#pragma once

// schedule_on -- start a task somewhere else. It wraps a task so the
// body first hops to a scheduler: co_await schedule_on(pool, t) parks
// the awaiting coroutine, lets the scheduler resume it, and only then
// starts t -- so t runs on the scheduler rather than wherever the
// awaiter happened to be.
//
//   co_await coro::schedule_on(pool, make_request());   // body on pool
//
// `task | schedule_on(sched)` is the pipe form; the one-argument
// overload returns a transform that can be named once and piped
// through several tasks. The wrapper is one extra coroutine frame that
// owns the child, so the pair travels as a single task.

#include <type_traits>
#include <utility>

#include "coro/concepts/scheduler.h"
#include "coro/task.h"

namespace coro {
    // Parks the caller on sched.schedule(), then runs t to completion
    // inline. t is moved into the wrapper's frame -- after that the
    // wrapper owns it and the caller must not await it again.
    //
    // Constrained on SCHEDULER as deduced -- `const pool` for a const
    // argument -- not on a cv-stripped copy of it. schedule() is not const,
    // so stripping the const would admit a const scheduler at the signature
    // and only reject it inside the body, where a caller's own
    // requires-clause cannot see the failure.
    template <typename SCHEDULER, typename T> requires scheduler<SCHEDULER>
    [[nodiscard]] auto schedule_on(SCHEDULER& sched, task<T> t) -> task<T> {
        co_await sched.schedule();
        if constexpr (std::is_void_v<T>) {
            co_await std::move(t);
        } else {
            co_return co_await std::move(t);
        }
    }

    // The pipe form's carrier: just a reference to the scheduler. It
    // exists so `task | schedule_on(sched)` works without allocating
    // anything or knowing the task's type up front.
    template <typename SCHEDULER>
    struct schedule_on_transform {
        explicit schedule_on_transform(SCHEDULER& sched) noexcept : scheduler(sched) {
        }

        SCHEDULER& scheduler;
    };

    template <typename SCHEDULER> requires scheduler<SCHEDULER>
    [[nodiscard]] schedule_on_transform<SCHEDULER> schedule_on(SCHEDULER& sched) noexcept {
        return schedule_on_transform<SCHEDULER>{sched};
    }

    template <typename T, typename SCHEDULER>
    [[nodiscard]] auto operator|(T&& value, const schedule_on_transform<SCHEDULER>& transform)
        -> decltype(schedule_on(transform.scheduler, std::forward<T>(value))) {
        return schedule_on(transform.scheduler, std::forward<T>(value));
    }
}
