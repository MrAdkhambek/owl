#pragma once

// A delayed_scheduler backed by a private timer thread. schedule()
// hands the coroutine to that thread to resume; schedule_after parks it
// until the deadline passes and resumes it there. The awaiters are
// trivial views over the queue -- the queue's thread does all of the
// waking, so this class holds no synchronisation of its own.

#include <coroutine>

#include "coro/executors/detail/timed_wait_queue.h"
#include "coro/concepts/delayed_scheduler.h"

namespace coro {
    // Self-contained: constructing one starts its timer thread,
    // destroying it joins that thread (see timed_wait_queue). Awaiters
    // capture a raw pointer to the private queue, so the object must
    // not be copied around while coroutines are parked on it.
    //
    // Destroy it only once nothing is parked on it. Whatever still is
    // gets woken early by the destructor, and if it then sleeps again
    // the co_await throws std::logic_error rather than parking on a
    // queue that is going away -- the way a periodic loop parked here
    // gets to end.
    class timer_scheduler {
    public:
        using clock = detail::timed_wait_queue::clock;

        timer_scheduler() = default;

        timer_scheduler(const timer_scheduler&) = delete;
        timer_scheduler& operator=(const timer_scheduler&) = delete;

        // A zero-delay park: always suspends, and the timer thread does
        // the resuming -- so this is a genuine hop to that thread,
        // never an inline resume.
        auto schedule() noexcept {
            struct awaiter {
                detail::timed_wait_queue* timers;

                bool await_ready() const noexcept {
                    return false;
                }

                void await_suspend(const std::coroutine_handle<> continuation) const {
                    timers->enqueue(clock::now(), continuation);
                }

                void await_resume() const noexcept {
                }
            };
            return awaiter{.timers = &timers_};
        }

        // Parks until the delay has elapsed, then resumes on the timer
        // thread. A non-positive delay never suspends: await_ready is
        // true and the coroutine runs on exactly where it is.
        auto schedule_after(const clock::duration delay) noexcept {
            struct awaiter {
                detail::timed_wait_queue* timers;
                clock::duration delay;

                bool await_ready() const noexcept {
                    return delay <= clock::duration::zero();
                }

                void await_suspend(const std::coroutine_handle<> continuation) const {
                    timers->enqueue(clock::now() + delay, continuation);
                }

                void await_resume() const noexcept {
                }
            };
            return awaiter{.timers = &timers_, .delay = delay};
        }

    private:
        // The queue's thread resumes parked coroutines inline -- no
        // reposting, no indirection -- so a coroutine sleeping here
        // *runs* on the timer thread once its deadline passes.
        detail::timed_wait_queue timers_{
            [](const std::coroutine_handle<> continuation) {
                continuation.resume();
            }
        };
    };

    static_assert(delayed_scheduler<timer_scheduler>);
}
