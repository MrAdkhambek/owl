#pragma once

// The contract for "somewhere a coroutine can wait". Extends scheduler
// with schedule_after, a timed park: the coroutine resumes at or after
// the delay, back on the scheduler. steady_clock is named explicitly so
// durations are monotonic at every call site -- no wall-clock surprises
// -- and so implementations share one clock vocabulary.

#include <chrono>

#include "coro/concepts/scheduler.h"

namespace coro {
    // scheduler<S> is required alongside schedule_after: a delayed
    // scheduler is still a scheduler, and the two needs travel together
    // (co_await sched.schedule() to get there, schedule_after to sleep).
    template <typename S>
    concept delayed_scheduler = scheduler<S> && requires(S& instance, std::chrono::steady_clock::duration delay) {
        { instance.schedule_after(delay) };
    };
}
