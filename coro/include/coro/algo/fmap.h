#pragma once

// fmap -- the coroutine analogue of map/transform. It applies a function to
// the value a task produces, or to every element a generator yields, and
// hands the mapped value(s) back as the same kind of object (a task or a
// generator). The function may itself be async: if it returns a task, that
// task is awaited before its result is yielded.
//
//   auto name   = fmap(std::move(user_id), [](int id) { return load_name(id); });
//   // load_name returns task<std::string>, so name is task<std::string>
//
//   auto labels = std::move(evens) | fmap([](int n) { return std::to_string(n); });
//   // async_generator<std::string>
//
// `gen | fmap(fn)` is the pipe form of fmap(gen, fn); the transform can also
// be named once and piped through several values.

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

#include "coro/async_generator.h"
#include "coro/task.h"

namespace coro {
    namespace detail {
        // Peels a task<T> down to its T. func may return a task (it does async
        // work); transform_value_t needs the underlying value type, so the
        // wrapper is stripped here.
        template <typename R>
        struct unwrapped {
            using type = R;
        };

        template <typename R>
        struct unwrapped<task<R>> {
            using type = R;
        };
    }

    // The result element type of a mapped value: func's return type, with any
    // wrapping task<> removed so an async func reports its payload, not the task.
    template <typename F, typename... Args>
    using transform_value_t = detail::unwrapped<std::invoke_result_t<F&, Args...>>::type;

    // Maps the value of a single task through func. func is called with the
    // awaited value (moved); if func returns a task, that task is awaited too.
    //
    //   auto name = fmap(std::move(user_id), [](int id) { return load_name(id); });
    //   // load_name returns task<std::string>, so name is task<std::string>
    template <typename F, typename T> requires std::invocable<F&, T&&>
    [[nodiscard]] auto fmap(task<T> t, F func) -> task<transform_value_t<F, T>> {
        if constexpr (is_task<std::invoke_result_t<F&, T&&>>) {
            co_return co_await std::invoke(func, co_await std::move(t));
        } else {
            co_return std::invoke(func, co_await std::move(t));
        }
    }

    // The void-task case: run t for its effect (and to await its exception),
    // then produce func's value. func takes no argument.
    //
    //   auto ready = fmap(std::move(setup), [] { return 42; });
    //   // awaits setup, then yields 42 -> task<int>
    template <typename F> requires std::invocable<F&>
    [[nodiscard]] auto fmap(task<> t, F func) -> task<transform_value_t<F>> {
        co_await std::move(t);
        if constexpr (is_task<std::invoke_result_t<F&>>) {
            co_return co_await std::invoke(func);
        } else {
            co_return std::invoke(func);
        }
    }

    // Maps every element of a generator through func, lazily and in order.
    // func is called with each element (moved); if it returns a task, that
    // task is awaited before the element is yielded.
    //
    //   auto labels = fmap(std::move(evens), [](int n) { return std::to_string(n); });
    //   // async_generator<std::string>
    template <typename F, typename T> requires std::invocable<F&, T&&> && std::is_object_v<transform_value_t<F, T&&>>
    [[nodiscard]] auto fmap(async_generator<T> gen, F func) -> async_generator<transform_value_t<F, T&&>> {
        while (auto v = co_await gen.next()) {
            if constexpr (is_task<std::invoke_result_t<F&, T&&>>) {
                co_yield co_await std::invoke(func, std::move(*v));
            } else {
                co_yield std::invoke(func, std::move(*v));
            }
        }
    }

    // The right-hand side of the pipe: a named, reusable transform. `gen | t`
    // applies t.func to gen. Holding func in a named object is what lets one
    // transform be piped through several values without being moved from.
    template <typename FUNC>
    struct fmap_transform {
        template <typename G> requires std::constructible_from<FUNC, G&&>
        explicit fmap_transform(G&& g) noexcept(std::is_nothrow_constructible_v<FUNC, G&&>) : func(std::forward<G>(g)) {
        }

        FUNC func;
    };

    namespace detail {
        // Pipes a source through a transform: fmap(source, transform.func).
        // An rvalue transform consumes func (moved); a const lvalue leaves it
        // in place so the same transform can be reused across pipes.
        template <typename FUNC, typename SOURCE>
        auto apply_transform(fmap_transform<FUNC>&& t, SOURCE&& s) -> decltype(fmap(std::forward<SOURCE>(s), std::move(t.func))) {
            return fmap(std::forward<SOURCE>(s), std::move(t.func));
        }

        template <typename FUNC, typename SOURCE>
        auto apply_transform(const fmap_transform<FUNC>& t, SOURCE&& s) -> decltype(fmap(std::forward<SOURCE>(s), t.func)) {
            return fmap(std::forward<SOURCE>(s), t.func);
        }
    }

    // The curried form of fmap: build the transform first, then pipe values
    // through it. This is what the right side of `x | fmap(fn)` produces.
    //
    //   auto as_string = fmap([](int n) { return std::to_string(n); });
    //   auto a = std::move(one) | as_string;
    //   auto b = std::move(two) | as_string;   // same transform, reused
    template <typename FUNC>
    [[nodiscard]] auto fmap(FUNC&& func) -> fmap_transform<std::decay_t<FUNC>> {
        return fmap_transform<std::decay_t<FUNC>>{std::forward<FUNC>(func)};
    }

    // Pipe a value (task or generator) through a transform: `value | t` is
    // fmap(value, t.func). Three overloads cover rvalue, lvalue, and const
    // lvalue transforms so a named transform can be reused across pipes.
    //
    //   auto labels = std::move(evens) | fmap([](int n) { return std::to_string(n); });
    //   // async_generator<std::string>
    template <typename T, typename FUNC>
    [[nodiscard]] auto operator|(T&& value, fmap_transform<FUNC>&& transform)
        -> decltype(detail::apply_transform(std::move(transform), std::forward<T>(value))) {
        return detail::apply_transform(std::move(transform), std::forward<T>(value));
    }

    template <typename T, typename FUNC>
    [[nodiscard]] auto operator|(T&& value, fmap_transform<FUNC>& transform)
        -> decltype(detail::apply_transform(transform, std::forward<T>(value))) {
        return detail::apply_transform(transform, std::forward<T>(value));
    }

    template <typename T, typename FUNC>
    [[nodiscard]] auto operator|(T&& value, const fmap_transform<FUNC>& transform)
        -> decltype(detail::apply_transform(transform, std::forward<T>(value))) {
        return detail::apply_transform(transform, std::forward<T>(value));
    }
}
