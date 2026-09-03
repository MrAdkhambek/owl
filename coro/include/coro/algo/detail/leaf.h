#pragma once

// The engine behind the borrow-based combinators: where one child's outcome
// lands (slot), the coroutine that drives exactly one child and reports back
// (leaf), and its body (run_child). Private to when_all.h and when_any.h,
// which are its only callers.

#include <coroutine>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>

#include "coro/concepts/awaitable.h"

namespace coro::detail {
    // Where one child's outcome lands. Owned by the combinator, not by the
    // coroutine that fills it -- when_any destroys the losing children, and
    // a result stored in a destroyed frame would go with them.
    template <typename T>
    struct slot {
        std::optional<T> value;
        std::exception_ptr error;
    };

    // Drives exactly one child, and tells the combinator when it is done.
    //
    // A coroutine is needed at all because these awaitables are lazy and
    // single-consumer: task<T> does not start until awaited, and only one
    // coroutine can await it. Concurrency therefore means one driver each,
    // which is what turns N sequential co_awaits into N overlapping ones.
    //
    // It parks at final_suspend rather than ending, because the combinator
    // owns it: the last thing it does is hand control to whoever should run
    // next, and the frame is reclaimed by ~leaf afterwards.
    template <typename State>
    class leaf {
    public:
        struct promise_type {
            leaf get_return_object() noexcept {
                return leaf{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            // ReSharper disable once CppMemberFunctionMayBeStatic
            [[nodiscard]] std::suspend_always initial_suspend() const noexcept {
                return {};
            }

            struct final_awaiter {
                // ReSharper disable once CppMemberFunctionMayBeStatic
                [[nodiscard]] bool await_ready() const noexcept {
                    return false;
                }

                // Symmetric transfer to whatever the combinator says runs
                // next -- the parent if this was the child it was waiting
                // for, otherwise nothing. Never resume(), so N children
                // completing in a chain do not grow the stack by N frames.
                std::coroutine_handle<> await_suspend(
                    const std::coroutine_handle<promise_type> self) noexcept {
                    return self.promise().state->child_finished(self.promise().index);
                }

                // ReSharper disable once CppMemberFunctionMayBeStatic
                void await_resume() const noexcept {
                }
            };

            // ReSharper disable once CppMemberFunctionMayBeStatic
            [[nodiscard]] final_awaiter final_suspend() const noexcept {
                return {};
            }

            void return_void() const noexcept {
            }

            // Unreachable: the body catches everything before it can escape,
            // which is the point -- a child's exception belongs in its slot,
            // where the combinator can decide what to do with it.
            void unhandled_exception() const noexcept {
            }

            State* state = nullptr;
            std::size_t index = 0;
        };

        leaf() noexcept = default;

        explicit leaf(const std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {
        }

        leaf(const leaf&) = delete;
        leaf& operator=(const leaf&) = delete;

        leaf(leaf&& other) noexcept : handle_(std::exchange(other.handle_, {})) {
        }

        leaf& operator=(leaf&& other) noexcept {
            if (this != &other) {
                if (handle_) handle_.destroy();
                handle_ = std::exchange(other.handle_, {});
            }
            return *this;
        }

        ~leaf() {
            if (handle_) handle_.destroy();
        }

        // Said after construction rather than passed in, because the
        // combinator that owns the state is still being built when the
        // coroutine is created.
        void bind(State* const state, const std::size_t index) noexcept {
            handle_.promise().state = state;
            handle_.promise().index = index;
        }

        void start() const {
            if (handle_ && !handle_.done()) handle_.resume();
        }

    private:
        std::coroutine_handle<promise_type> handle_;
    };

    // The body of a leaf. Free rather than a member because a coroutine
    // taking the awaitable by forwarding reference is what lets a
    // non-movable awaiter -- sleep_for, above all -- be combined at all:
    // the frame stores the reference, and the referent stays where the
    // caller put it.
    template <typename State, typename T, typename Aw>
    leaf<State> run_child(slot<T>& slot, Aw&& awaitable) {
        try {
            if constexpr (std::is_void_v<await_result_t<Aw>>) {
                co_await std::forward<Aw>(awaitable);
                slot.value.emplace();
            } else {
                slot.value.emplace(co_await std::forward<Aw>(awaitable));
            }
        } catch (...) {
            slot.error = std::current_exception();
        }
    }
}
