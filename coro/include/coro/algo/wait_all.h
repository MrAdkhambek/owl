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
//
// It is when_all, restricted to task<T> children and wrapped in a task of
// its own. The wrapper is the point: a wait_all is itself a task, so it
// nests into every task-based algorithm unchanged -- handed to another
// wait_all, piped through fmap, started on a pool with schedule_on -- at the
// price of one extra frame. A coroutine that just wants the join and no
// wrapper awaits when_all directly. Sharing the engine also means the two
// agree on everything else: fail-late (every child runs; the first failing
// ARGUMENT's exception is the one rethrown), and one void_value type.

#include <tuple>
#include <type_traits>
#include <utility>

#include "coro/algo/when_all.h"
#include "coro/concepts/awaitable.h"
#include "coro/task.h"

namespace coro {
    // The element type a task<T> contributes to the result tuple: T, or
    // void_value when T is void.
    template <typename T>
    using wait_component_t = std::conditional_t<std::is_void_v<T>, void_value, T>;

    // Runs every task in ts concurrently and, once all of them have completed,
    // returns their results as a tuple (in the order given). A void task
    // contributes a void_value slot.
    //
    //   auto [user, posts] = co_await wait_all(fetch_user(id), fetch_posts(id));
    //   // both fetches ran in parallel; user and posts are both ready
    template <typename... Ts>
    [[nodiscard]] auto wait_all(task<Ts>... ts) -> task<std::tuple<wait_component_t<Ts>...>> {
        co_return co_await when_all(std::move(ts)...);
    }
}
