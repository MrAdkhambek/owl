#pragma once

// as_tuple -- catch, don't throw. It wraps an awaitable and hands back
// a (exception_ptr, value) tuple instead of propagating the exception:
// success is {nullptr, value}, failure is {eptr, Result{}} with a
// value-initialised slot standing in for the value that never happened.
//
// It adapts the wrapped awaiter directly (see
// result/detail/awaiter_adapter.h), so unlike the task-returning
// as_result(task<T>) it adds no coroutine frame at all.

#include <exception>
#include <tuple>
#include <type_traits>
#include <utility>

#include "coro/concepts/awaitable.h"
#include "coro/result/detail/awaiter_adapter.h"

namespace coro {
    // Result is what the child produces; Tuple is what you get back.
    // A void child yields a one-element tuple, since there is no value
    // to report either way.
    template <typename Aw>
    class [[nodiscard]] as_tuple_t : public detail::awaiter_adapter<Aw> {
    public:
        using Result = await_result_t<Aw>;
        using Tuple = std::conditional_t<
            std::is_void_v<Result>,
            std::tuple<std::exception_ptr>,
            std::tuple<std::exception_ptr, std::conditional_t<std::is_void_v<Result>, void_value, Result>>>;

        using detail::awaiter_adapter<Aw>::awaiter_adapter;

        // The single interception point: whatever the child throws is
        // caught here and demoted to data. On failure the value slot is
        // default-constructed, which is why Result must be default
        // constructible -- a type that cannot be is as_result's job.
        Tuple await_resume() {
            try {
                if constexpr (std::is_void_v<Result>) {
                    this->awaiter_.await_resume();
                    return Tuple{nullptr};
                } else {
                    static_assert(std::is_default_constructible_v<Result>,
                                  "as_tuple must value-initialise the result slot when the child fails; "
                                  "use as_result for a type that cannot be default-constructed");
                    return Tuple{nullptr, this->awaiter_.await_resume()};
                }
            } catch (...) {
                if constexpr (std::is_void_v<Result>) return Tuple{std::current_exception()};
                else return Tuple{std::current_exception(), Result{}};
            }
        }
    };

    template <awaitable Aw>
    [[nodiscard]] as_tuple_t<Aw> as_tuple(Aw&& awaitable) {
        return as_tuple_t<Aw>{std::forward<Aw>(awaitable)};
    }
}

