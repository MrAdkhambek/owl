#pragma once

// resume_on -- move the continuation, not the work. Unlike schedule_on,
// which starts a task on a scheduler, resume_on lets the task run where
// it would have anyway and transfers back afterwards: the code
// following the co_await resumes on the scheduler's thread.
//
//   int n = co_await coro::resume_on(pool, add(2, 3));   // n ready on pool
//
// The usual use is returning to a known context after work that may
// have completed elsewhere. Same two forms as schedule_on: direct and
// pipe (`task | resume_on(sched)`).

#include <type_traits>
#include <utility>

#include "coro/concepts/scheduler.h"
#include "coro/task.h"

namespace coro {
    // Runs t to completion wherever it lands, then hops to sched before
    // handing the result back. t is moved into the wrapper's frame, and
    // the result is stored there across the hop.
    //
    // Constrained on SCHEDULER as deduced -- `const pool` for a const
    // argument -- not on a cv-stripped copy of it. schedule() is not const,
    // so stripping the const would admit a const scheduler at the signature
    // and only reject it inside the body, where a caller's own
    // requires-clause cannot see the failure.
    template <typename SCHEDULER, typename T> requires scheduler<SCHEDULER>
    [[nodiscard]] auto resume_on(SCHEDULER& sched, task<T> t) -> task<T> {
        if constexpr (std::is_void_v<T>) {
            co_await std::move(t);
            co_await sched.schedule();
        } else {
            T result = co_await std::move(t);
            co_await sched.schedule();
            co_return result;
        }
    }

    // The pipe form's carrier: just a reference to the scheduler, so
    // `task | resume_on(sched)` works without knowing the task's type.
    template <typename SCHEDULER>
    struct resume_on_transform {
        explicit resume_on_transform(SCHEDULER& sched) noexcept : scheduler(sched) {
        }

        SCHEDULER& scheduler;
    };

    template <typename SCHEDULER> requires scheduler<SCHEDULER>
    [[nodiscard]] resume_on_transform<SCHEDULER> resume_on(SCHEDULER& sched) noexcept {
        return resume_on_transform<SCHEDULER>{sched};
    }

    template <typename T, typename SCHEDULER>
    [[nodiscard]] auto operator|(T&& value, const resume_on_transform<SCHEDULER>& transform)
        -> decltype(resume_on(transform.scheduler, std::forward<T>(value))) {
        return resume_on(transform.scheduler, std::forward<T>(value));
    }
}
