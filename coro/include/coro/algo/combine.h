#pragma once

// Element-wise pairing of two async generators -- the coroutine analogue of
// zip. A step pulls one element from each side at once and yields the pair,
// stopping when the shorter side runs out, so it yields
// min(len(left), len(right)) pairs. Both sides run lazily and in parallel,
// and the longer side's surplus is simply never pulled.
//
//   auto pairs = combine(std::move(keys), std::move(values));
//   // async_generator<std::tuple<K, V>>: (k1, v1), (k2, v2), ...
//
// A func overload folds each pair into a single value, and `left | right`
// is sugar for combine(left, right) so the result chains straight into fmap.

#include <concepts>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

#include "coro/task.h"
#include "coro/algo/fmap.h"
#include "coro/algo/wait_all.h"
#include "coro/async_generator.h"

namespace coro {
    namespace detail {
        // Pulls a single element off a generator and hands it back as a task,
        // so two independent pulls can run side by side through wait_all.
        // The result is nullopt when the generator is exhausted, which is how
        // combine learns that a side has run out.
        template <typename T>
        auto pull_one(async_generator<T>& gen) -> task<std::optional<T>> {
            co_return co_await gen.next();
        }
    }

    // Pairs the two generators element by element (zip). Each step co_awaits
    // both sides at once -- see wait_all -- and yields (left, right) while
    // both still have an element; the first exhausted side ends the stream.
    //
    //   auto keys   = ...;   // async_generator<std::string>
    //   auto values = ...;   // async_generator<int>
    //   auto pairs  = combine(std::move(keys), std::move(values));
    //   // async_generator<std::tuple<std::string, int>>: (k1, v1), (k2, v2), ...
    template <typename T, typename Y>
    [[nodiscard]] auto combine(async_generator<T> left, async_generator<Y> right) -> async_generator<std::tuple<T, Y>> {
        while (true) {
            auto [first, second] = co_await wait_all(detail::pull_one(left), detail::pull_one(right));
            if (!first || !second) break;
            co_yield std::tuple<T, Y>{std::move(*first), std::move(*second)};
        }
    }

    // The combining step, but each pair is folded through func into a single
    // value of type R before it is yielded. func takes the two elements (as
    // T&&, Y&&); if it returns a task, the task is awaited, so func may itself
    // do async work.
    //
    //   auto rows = combine(std::move(keys), std::move(values),
    //                       [](std::string k, int v) { return k + "=" + std::to_string(v); });
    //   // async_generator<std::string>: "a=1", "b=2", ...
    template <typename F, typename T, typename Y, typename R = transform_value_t<F, T&&, Y&&>> requires std::invocable<F&, T&&, Y&&> &&
        std::is_object_v<R>
    [[nodiscard]] auto combine(async_generator<T> left, async_generator<Y> right, F func) -> async_generator<R> {
        return combine(std::move(left), std::move(right))
            | fmap([f = std::move(func)](std::tuple<T, Y> row) mutable -> std::invoke_result_t<F&, T&&, Y&&> {
                return std::apply(f, std::move(row));
            });
    }

    // Pipe sugar: `left | right` is combine(left, right). This lets a paired
    // generator feed straight into a fmap step, one pipe after another:
    //
    //   auto rows = std::move(keys) | std::move(values)
    //              | fmap([](std::tuple<std::string, int> kv) {
    //                        return std::get<0>(kv) + "=" + std::to_string(std::get<1>(kv));
    //                    });
    //   // async_generator<std::string>: "a=1", "b=2", ...
    template <typename T, typename Y>
    [[nodiscard]] auto operator|(
        async_generator<T>&& left,
        async_generator<Y>&& right
    ) -> decltype(combine(std::move(left), std::move(right))) {
        return combine(std::move(left), std::move(right));
    }
}
