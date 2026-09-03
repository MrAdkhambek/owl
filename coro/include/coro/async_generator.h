#pragma once

// A stream of values produced lazily by a coroutine and pulled by the
// consumer: each co_yield hands control (and one value) back to
// whoever is awaiting next(), which resumes the producer on the
// following pull. The stream ends when the body returns -- next()
// reports that as nullopt -- and a body exception is rethrown from
// next(), at the pull site.
//
// The protocol is strictly lockstep: producer and consumer alternate,
// so a generator never runs ahead of its consumer. Like task, the
// frame starts suspended and lives until the generator is destroyed --
// destroying a half-consumed generator destroys the frame, with the
// usual consequence that the producer must not outlive anything it
// references.

#include <cassert>
#include <concepts>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

namespace coro {
    namespace detail {

        // The awaiter returned by yield_value and final_suspend: both
        // suspend the producer and resume the consumer by symmetric
        // transfer. init consumer_ to noop so a yield with nobody
        // registered resumes into no one rather than crashing.
        struct resume_consumer {
            std::coroutine_handle<> consumer;

            bool await_ready() const noexcept {
                return false;
            }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<>) const noexcept {
                return consumer;
            }

            void await_resume() const noexcept {
            }
        };

        // The courier between producer and consumer. The producer
        // writes one element (or an exception, or "finished") here
        // before handing control back; the consumer reads it in
        // take(). value_ points at the yielded object, which lives in
        // the producer's frame and is valid exactly until the next
        // pull -- hence take() copies or moves out immediately.
        template <typename T>
        class element_slot {
            const T* value_{};

            // Non-null only when the element was yielded as an rvalue
            // and may be moved from. A copyable T yielded as an lvalue
            // is always copied; a move is consent, granted per yield.
            T* movable_{};

            std::exception_ptr e_{};

            std::coroutine_handle<> consumer_{std::noop_coroutine()};

        public:

            void set_value(const T& v) noexcept requires std::copy_constructible<T> {
                value_ = std::addressof(v);
                movable_ = nullptr;
            }

            void set_movable_value(T& v) noexcept {
                value_ = std::addressof(v);
                movable_ = std::addressof(v);
            }

            void set_exception(std::exception_ptr e) noexcept {
                e_ = e;
            }

            void set_finished() noexcept {
                value_ = nullptr;
                movable_ = nullptr;
            }

            void set_consumer(std::coroutine_handle<> c) noexcept {
                consumer_ = c;
            }

            [[nodiscard]] std::coroutine_handle<>
            consumer() const noexcept {
                return consumer_;
            }

            // Delivers the element staged by the last yield: moves when
            // consent was given (or T is move-only), copies otherwise,
            // rethrows a staged exception, and reports end-of-stream as
            // nullopt.
            std::optional<T> take() {
                if (e_) std::rethrow_exception(std::exchange(e_, {}));
                if (!value_) return std::nullopt;
                if constexpr (std::copy_constructible<T>) {
                    if (!movable_) return std::optional<T>{*value_};
                }
                assert(movable_ != nullptr && "element offered without consent to move it");
                return std::optional<T>{std::move(*movable_)};
            }
        };

        // Supplies the co_yield half of the promise interface: the
        // value is staged, then the producer suspends and transfers
        // back to the consumer. Like task's returns, it is the part
        // that differs from promise_type's other duties.
        template <typename T>
        struct yields : element_slot<T> {

            resume_consumer yield_value(const T& v) noexcept requires std::copy_constructible<T> {
                this->set_value(v);
                return resume_consumer{this->consumer()};
            }

            resume_consumer yield_value(T&& v) noexcept {
                this->set_movable_value(v);
                return resume_consumer{this->consumer()};
            }
        };
    }

    // The consumer-facing type. Hold it, call next() in a loop, and let
    // it go out of scope to tear the frame down.
    template <typename T>
    class [[nodiscard]] async_generator {
        static_assert(std::is_object_v<T>, "async_generator requires an object element type");

    public:

        struct promise_type : detail::yields<T> {

            async_generator get_return_object() noexcept {
                return async_generator{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            // Lazy, like task: the body runs only when pulled.
            std::suspend_always initial_suspend() const noexcept {
                return {};
            }

            void unhandled_exception() noexcept {
                this->set_exception(std::current_exception());
            }

            void return_void() const noexcept {
            }

            // Body finished: mark the slot finished and transfer back
            // to the consumer, whose next() will then see nullopt. The
            // frame stays suspended here until the generator destroys
            // it.
            detail::resume_consumer final_suspend() noexcept {
                this->set_finished();
                return detail::resume_consumer{this->consumer()};
            }
        };

        using handle_type = std::coroutine_handle<promise_type>;
        using value_type = T;

        // Pulls the next element. Awaiting it runs the producer up to
        // its next yield (or return, or throw); the result is the
        // yielded value, nullopt at end of stream, or the producer's
        // exception. Safe on an empty (default-constructed or moved
        // from) generator: it reports nullopt without suspending.
        auto next() noexcept {
            struct pull {
                handle_type h;

                bool await_ready() const noexcept {
                    return !h || h.done();
                }

                // Registers this consumer, then transfers straight
                // into the producer -- the caller's frame is parked
                // until the producer yields, finishes, or throws.
                std::coroutine_handle<> await_suspend(std::coroutine_handle<> consumer) const noexcept {
                    h.promise().set_consumer(consumer);
                    return h;
                }

                std::optional<T> await_resume() const {
                    if (!h) return std::nullopt;
                    return h.promise().take();
                }
            };

            return pull{h_};
        }

        async_generator() noexcept = default;

        async_generator(async_generator&& o) noexcept : h_(std::exchange(o.h_, {})) {
        }

        async_generator& operator=(async_generator&& o) noexcept {
            if (this != &o) {
                if (h_) h_.destroy();
                h_ = std::exchange(o.h_, {});
            }
            return *this;
        }

        async_generator(const async_generator&) = delete;
        async_generator& operator=(const async_generator&) = delete;

        // Destroys the frame wherever it is parked -- mid-yield,
        // mid-pull, finished, or never started. Nothing is resumed:
        // the producer's remaining body simply never runs.
        ~async_generator() {
            if (h_) h_.destroy();
        }

        [[nodiscard]] bool done() const noexcept {
            return !h_ || h_.done();
        }

        explicit operator bool() const noexcept {
            return static_cast<bool>(h_);
        }

    private:

        explicit async_generator(handle_type h) noexcept : h_(h) {
        }

        handle_type h_{};
    };
}
