#pragma once

// What it means to be awaitable, and what awaiting one produces.
//
// The standard allows three spellings of "awaitable" -- a member
// operator co_await, a free one, or an awaiter already -- so anything
// generic over awaitables has to accept all three. get_awaiter is the
// single place that normalises them; await_result_t and await_value_t
// are written in terms of it, which is why it lives here with the
// concept rather than with the combinators that use it.

#include <concepts>
#include <coroutine>
#include <type_traits>
#include <utility>

namespace coro {
    // Stands in for void wherever a result must be a value: the Nth slot of a
    // when_all tuple, the Nth alternative of a when_any variant.
    //
    // Not std::monostate, which reads as "this variant is empty" and would be
    // actively misleading beside a real alternative. Two void_values in one variant
    // are still distinguishable -- variant indices are positional -- so
    // when_any(sleep(1s), sleep(2s)) can still say which fired.
    struct void_value {
        bool operator==(const void_value &) const noexcept = default;
    };

    namespace detail {
        // The standard allows three spellings of "awaitable": a member
        // operator co_await, a free one, or an awaiter already. A combinator
        // has to accept all three, because all three occur in practice: task<T> has
        // the member, a timer awaiter is one outright.
        template<typename T>
        decltype(auto) get_awaiter(T &&value) {
            if constexpr (requires { std::forward<T>(value).operator co_await(); }) {
                return std::forward<T>(value).operator co_await();
            } else if constexpr (requires { operator co_await(std::forward<T>(value)); }) {
                return operator co_await(std::forward<T>(value));
            } else {
                return std::forward<T>(value); // already an awaiter; forwarded, never copied
            }
        }

        template<typename T>
        using awaiter_t = std::remove_reference_t<decltype(get_awaiter(std::declval<T>()))>;
    }

    // What await_resume gives back. Names the result type of `co_await a`.
    template<typename T>
    using await_result_t = decltype(std::declval<detail::awaiter_t<T> &>().await_resume());

    // The same, with void folded onto void_value, for the places a result is stored
    // rather than produced.
    template<typename T>
    using await_value_t = std::conditional_t<std::is_void_v<await_result_t<T> >, void_value, await_result_t<T> >;

    template<typename T>
    concept awaitable = requires(std::coroutine_handle<> waiting, detail::awaiter_t<T> &awaiter) {
        { awaiter.await_ready() } -> std::convertible_to<bool>;
        awaiter.await_suspend(waiting);
        awaiter.await_resume();
    };
}
