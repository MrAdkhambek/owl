#pragma once

// as_result -- a task's failure, delivered as data. Where as_tuple
// reports (eptr, value), as_result reports a coro::result<T>, which
// composes with expected-based code and asks nothing of T.
//
// Two overloads share the name: as_result(task<T>) returns a wrapper
// task -- a real frame, but one that is itself a task, so it nests into
// the task-based algorithms unchanged. as_result(any other awaitable)
// is a zero-frame adapter over the child's own awaiter. Partial
// ordering routes tasks to the first and everything else to the
// second; `| as_result()` picks the right one per argument.

#include <concepts>
#include <coroutine>
#include <exception>
#include <expected>
#include <type_traits>
#include <utility>

#include "coro/concepts/awaitable.h"
#include "coro/result/detail/awaiter_adapter.h"
#include "coro/result/result.h"
#include "coro/task.h"

namespace coro {

    // Wraps t in a new coroutine whose body is one try/catch: the
    // co_await happens inside the try, so the child is driven by this
    // frame rather than merely observed at its end. The frame is the
    // price of the nesting; see the adapter below for the frame-free
    // shape.
    template <typename T>
    [[nodiscard]] auto as_result(task<T> t) -> task<result<T>> {
        try {
            if constexpr (std::is_void_v<T>) {
                co_await std::move(t);
                co_return result<T>{};
            } else {
                co_return result<T>{co_await std::move(t)};
            }
        } catch (...) {
            co_return std::unexpected(std::current_exception());
        }
    }

    // The zero-allocation sibling of as_result(task<T>): an adapter around
    // any awaitable, adding no frame of its own. Partial ordering gives a
    // task the overload above, which is the one that can flatten.
    template <typename Aw>
    class [[nodiscard]] as_result_t : public detail::awaiter_adapter<Aw> {
    public:
        using Result = await_result_t<Aw>;
        using Expected = std::expected<Result, std::exception_ptr>;

        using detail::awaiter_adapter<Aw>::awaiter_adapter;

        Expected await_resume() {
            try {
                if constexpr (std::is_void_v<Result>) {
                    this->awaiter_.await_resume();
                    return Expected{};
                } else {
                    return Expected{this->awaiter_.await_resume()};
                }
            } catch (...) {
                return Expected{std::unexpected(std::current_exception())};
            }
        }
    };

    template <awaitable Aw>
    [[nodiscard]] as_result_t<Aw> as_result(Aw&& awaitable) {
        return as_result_t<Aw>{std::forward<Aw>(awaitable)};
    }

    // True when Try is exactly task<result<task_value_t<Throwing>>> for
    // a task Throwing -- the type as_result produces from it. Names the
    // "already error-proofed" shape, so algorithms can recognise
    // children that will not throw through them.
    template <typename Try, typename Throwing>
    concept as_result_of = is_task<Throwing> && std::same_as<std::remove_cvref_t<Try>, task<result<task_value_t<Throwing>>>>;

    // The pipe form's carrier. Stateless, so it costs nothing to pass
    // around; overload resolution against the task overload does the
    // dispatch.
    struct as_result_transform {
    };

    [[nodiscard]] inline auto as_result() noexcept -> as_result_transform {
        return {};
    }

    template <typename T>
    [[nodiscard]] auto operator|(task<T>&& t, as_result_transform) -> decltype(as_result(std::move(t))) {
        return as_result(std::move(t));
    }
}
