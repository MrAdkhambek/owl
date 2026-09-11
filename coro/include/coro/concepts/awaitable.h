#pragma once

// What it means to be awaitable, and what awaiting one produces.
//
// awaiter is the object the compiler talks to (ready / suspend / resume).
// awaitable is anything that can produce one: a member operator co_await,
// a free one, or an awaiter already. get_awaiter is the single place that
// normalises the three spellings; await_result_t and await_value_t are
// written in terms of it, which is why it lives here with the concepts
// rather than with the combinators that use it.

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
        bool operator==(const void_value&) const noexcept = default;
    };

    // Distinct from awaitable: task and co_await wrappers produce an awaiter
    // without being one. Combinators store the awaiter, not the awaitable.
    template <typename T>
    concept awaiter = requires(T& instance, std::coroutine_handle<> waiting) {
        { instance.await_ready() } -> std::convertible_to<bool>;
        instance.await_suspend(waiting);
        instance.await_resume();
    };

    namespace detail {
        // Value category matters: task's operator co_await is &&-qualified.
        template <typename T>
        concept has_member_co_await = requires(T&& value) {
            std::forward<T>(value).operator co_await();
        };

        template <typename T>
        concept has_free_co_await = requires(T&& value) {
            operator co_await(std::forward<T>(value));
        };

        template <typename T>
        decltype(auto) get_awaiter(T&& value) {
            if constexpr (has_member_co_await<T>) return std::forward<T>(value).operator co_await();
            else if constexpr (has_free_co_await<T>) return operator co_await(std::forward<T>(value));
            else return std::forward<T>(value); // already an awaiter; forwarded, never copied
        }

        template <typename T>
        using awaiter_t = std::remove_reference_t<decltype(get_awaiter(std::declval<T>()))>;

        // How a combinator or adapter keeps a child handed to it as Aw, the
        // deduced forwarding-reference type. An lvalue is borrowed: the
        // caller still owns it and keeps it alive. A movable rvalue is moved
        // in, so that a group built from temporaries can be named and
        // awaited later -- `auto g = when_all(f(), h()); co_await g;` --
        // instead of dangling the moment the statement that built it ends.
        // A non-movable rvalue (async_mutex::lock_operation, an as_result_t
        // over a bare awaiter) can only be borrowed, and must therefore be
        // awaited within the full-expression that created it.
        template <typename Aw>
        using child_storage_t = std::conditional_t<
            !std::is_lvalue_reference_v<Aw> && std::is_move_constructible_v<std::remove_cvref_t<Aw>>,
            std::remove_cvref_t<Aw>,
            Aw&&>;
    }

    // What await_resume gives back. Names the result type of `co_await a`.
    template <typename T>
    using await_result_t = decltype(std::declval<detail::awaiter_t<T>&>().await_resume());

    // The same, with void folded onto void_value, for the places a result is stored
    // rather than produced.
    template <typename T>
    using await_value_t = std::conditional_t<std::is_void_v<await_result_t<T>>, void_value, await_result_t<T>>;

    template <typename T>
    concept awaitable = awaiter<detail::awaiter_t<T>>;
}
