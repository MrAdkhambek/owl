#pragma once

// The engine behind when_all and when_any: where one child's outcome lands
// (slot), the coroutine that drives exactly one child and reports back
// (leaf), and its body (run_child). Private to when_all.h and when_any.h,
// which are its only callers; each decides for itself how a child is held
// (see child_storage_t and run_child's Stored parameter).

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "coro/concepts/awaitable.h"

namespace coro::detail {
    // Where one child's outcome lands. Owned by the combinator, not by the
    // coroutine that fills it, so the result outlives the driver that
    // produced it. A reference result is kept as a pointer, as task's own
    // result_slot does: std::optional cannot hold a reference.
    template <typename T>
    struct slot {
        using stored = std::conditional_t<std::is_reference_v<T>, std::remove_reference_t<T>*, T>;

        std::optional<stored> value;
        std::exception_ptr error;

        template <typename From>
        void set(From&& from) {
            if constexpr (std::is_reference_v<T>) value.emplace(std::addressof(from));
            else value.emplace(std::forward<From>(from));
        }

        void set() {
            value.emplace();
        }

        // Moves the result out (or re-forms the reference); only valid once
        // the child has completed without error.
        T take() {
            if constexpr (std::is_reference_v<T>) return static_cast<T>(**value);
            else return std::move(*value);
        }
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

    // The body of a leaf. Stored names how the frame keeps the child --
    // the combinator picks it with child_storage_t: by value for a movable
    // rvalue, so the child lives exactly as long as the leaf; by reference
    // for an lvalue or a non-movable rvalue, which stays where the caller
    // put it. Spelled as an explicit template argument rather than deduced,
    // because a by-value parameter deduced from a forwarding reference
    // would copy every lvalue.
    //
    // The cast re-forms the reference category Stored implies -- an rvalue
    // for an owned child, so a task's &&-qualified operator co_await can be
    // reached -- and collapses to an lvalue for a borrowed one.
    template <typename State, typename Stored, typename T>
    leaf<State> run_child(slot<T>& slot, Stored awaitable) {
        try {
            if constexpr (std::is_void_v<await_result_t<Stored>>) {
                co_await static_cast<Stored&&>(awaitable);
                slot.set();
            } else {
                slot.set(co_await static_cast<Stored&&>(awaitable));
            }
        } catch (...) {
            slot.error = std::current_exception();
        }
    }
}
