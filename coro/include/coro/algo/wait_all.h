#pragma once

// wait_all -- run a set of tasks concurrently and return their results
// together, once every one of them has finished. It's the coroutine analogue
// of a join / all-of: each task runs on its own, they don't wait on each
// other, and the caller is resumed exactly once when the slowest one
// completes. Results come back as a tuple, in the same order as the tasks.
//
//   auto [user, posts] = co_await wait_all(fetch_user(id), fetch_posts(id));
//   // both fetches ran in parallel; user and posts are both ready
//
//   co_await wait_all(log_a(), log_b());   // fire both, join on both
//
// A void task still occupies a slot in the tuple (a void_value token), so the
// number of elements always matches the number of tasks.

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

#include "coro/task.h"

namespace coro {
    namespace detail {
        // A placeholder for a void result. std::tuple cannot hold void, so a
        // void task is represented by this empty type in the result tuple.
        struct void_value {
        };
    }

    // The element type a task<T> contributes to the result tuple: T, or
    // void_value when T is void.
    template <typename T>
    using wait_component_t = std::conditional_t<std::is_void_v<T>, detail::void_value, T>;

    namespace detail {
        // Shared join bookkeeping: how many participants are still pending,
        // and who to wake once they're all done. pending_ starts at children +
        // 1 because the parent's own check is an arrival too; the last arrival
        // to drive it to zero is the one responsible for resuming the join.
        class join_state {
        public:
            explicit join_state(const std::size_t children) noexcept : pending_(children + 1) {
            }

            join_state(const join_state&) = delete;
            join_state& operator=(const join_state&) = delete;

            // Records the coroutine to resume once every arrival has been seen.
            void set_continuation(const std::coroutine_handle<> continuation) noexcept {
                continuation_.store(continuation, std::memory_order_release);
            }

            // One participant is done. Atomically decrements pending_; the
            // last arrival (the one that reaches zero) gets the continuation
            // back to wake, everyone else gets an empty handle.
            std::coroutine_handle<> arrive() noexcept {
                if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    return continuation_.load(std::memory_order_acquire);
                }
                return {};
            }

        private:
            std::atomic<std::size_t> pending_;
            std::atomic<std::coroutine_handle<>> continuation_{nullptr};
        };

        // One child of a join: owns a single child coroutine's handle.
        // start() begins it running; take() retrieves its result once it's done.
        template <typename T>
        class join_child {
        public:
            struct promise_type : returns<T> {
                join_state* state;

                template <typename Source>
                promise_type(Source&, join_state* s) noexcept : state(s) {
                }

                join_child get_return_object() noexcept {
                    return join_child{std::coroutine_handle<promise_type>::from_promise(*this)};
                }

                std::suspend_always initial_suspend() const noexcept {
                    return {};
                }

                void unhandled_exception() noexcept {
                    this->set_exception(std::current_exception());
                }

                struct finish {
                    bool await_ready() const noexcept {
                        return false;
                    }

                    // Runs on the child's completion: if this child is the last
                    // arrival, resume the join; otherwise hand off to nothing.
                    std::coroutine_handle<>
                    await_suspend(const std::coroutine_handle<promise_type> me) const noexcept {
                        if (const std::coroutine_handle<> wake = me.promise().state->arrive()) {
                            return wake;
                        }
                        return std::noop_coroutine();
                    }

                    void await_resume() const noexcept {
                    }
                };

                finish final_suspend() const noexcept {
                    return {};
                }
            };

            using handle_type = std::coroutine_handle<promise_type>;

            explicit join_child(const handle_type handle) noexcept : handle_(handle) {
            }

            join_child(join_child&& o) noexcept : handle_(std::exchange(o.handle_, {})) {
            }

            join_child(const join_child&) = delete;
            join_child& operator=(const join_child&) = delete;

            ~join_child() {
                if (handle_) handle_.destroy();
            }

            // Begins the child running (it started suspended).
            void start() const {
                handle_.resume();
            }

            // Retrieves the child's result; only valid once the child has run.
            T take() const {
                return handle_.promise().take();
            }

        private:
            handle_type handle_;
        };

        // Adapts a task into a join child. A void task yields a void_value
        // token so it can occupy a slot in the result tuple.
        template <typename T>
        join_child<wait_component_t<T>> run_join_child(task<T> source, [[maybe_unused]] join_state* state) {
            if constexpr (std::is_void_v<T>) {
                co_await std::move(source);
                co_return void_value{};
            } else {
                co_return co_await std::move(source);
            }
        }

        // The awaitable behind wait_all. On suspend it starts every child and
        // waits for them all to arrive; on resume it collects their results
        // into a tuple, in the original order.
        template <typename... Ts>
        class [[nodiscard]] join_group {
        public:
            explicit join_group(task<Ts>... ts) : children_{run_join_child(std::move(ts), &state_)...} {
            }

            join_group(const join_group&) = delete;
            join_group& operator=(const join_group&) = delete;

            bool await_ready() const noexcept {
                return false;
            }

            // Starts every child, then registers itself as the last possible
            // arrival; the join completes as soon as the final arrival lands.
            bool await_suspend(const std::coroutine_handle<> me) noexcept {
                state_.set_continuation(me);
                std::apply([](auto&... kids) {
                    (kids.start(), ...);
                }, children_);
                return !state_.arrive();
            }

            // Every child has finished; collect each result into the tuple.
            std::tuple<wait_component_t<Ts>...> await_resume() {
                return std::apply([](auto&... kids) {
                    return std::tuple<wait_component_t<Ts>...>{kids.take()...};
                }, children_);
            }

        private:
            join_state state_{sizeof...(Ts)};
            std::tuple<join_child<wait_component_t<Ts>>...> children_;
        };
    }

    // Runs every task in ts concurrently and, once all of them have completed,
    // returns their results as a tuple (in the order given). A void task
    // contributes a void_value slot.
    //
    //   auto [user, posts] = co_await wait_all(fetch_user(id), fetch_posts(id));
    //   // both fetches ran in parallel; user and posts are both ready
    template <typename... Ts>
    [[nodiscard]] auto wait_all(task<Ts>... ts) -> task<std::tuple<wait_component_t<Ts>...>> {
        co_return co_await detail::join_group<Ts...>(std::move(ts)...);
    }
}
