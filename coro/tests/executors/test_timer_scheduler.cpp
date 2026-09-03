#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <utility>

#include <coro/executors/static_thread_pool.h>
#include <coro/executors/detail/timed_wait_queue.h>
#include <coro/executors/timer_scheduler.h>
#include <coro/concepts/delayed_scheduler.h>
#include <coro/run/schedule_on.h>
#include <coro/task.h>
#include <coro/run/sync_wait.h>
#include <vector>

using namespace std::chrono_literals;

// Running work and waiting on a clock are separate schedulers here, and
// neither knows the other exists.
static_assert(coro::scheduler<coro::static_thread_pool>);
static_assert(coro::postable<coro::static_thread_pool>);
static_assert(!coro::delayed_scheduler<coro::static_thread_pool>, "the pool must NOT carry timing");
static_assert(coro::scheduler<coro::timer_scheduler>);
static_assert(coro::delayed_scheduler<coro::timer_scheduler>);

namespace {
    // A permanently suspended coroutine frame: initial_suspend is
    // suspend_always and these tests never resume it. Move-only -- two live
    // copies of one handle would mean a double destroy().
    struct frame {
        struct promise_type {
            frame get_return_object() noexcept {
                return frame{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            std::suspend_always initial_suspend() const noexcept {
                return {};
            }

            // Unreachable here -- the frame is never resumed -- but a promise
            // needs it to compile.
            std::suspend_never final_suspend() const noexcept {
                return {};
            }

            void return_void() const noexcept {
            }

            void unhandled_exception() const noexcept {
                std::terminate();
            }
        };

        explicit frame(std::coroutine_handle<promise_type> h) noexcept : handle(h) {
        }

        frame(frame&& o) noexcept : handle(std::exchange(o.handle, {})) {
        }

        frame(const frame&) = delete;
        frame& operator=(const frame&) = delete;

        ~frame() {
            if (handle) handle.destroy();
        }

        std::coroutine_handle<promise_type> handle;
    };

    frame park() {
        co_return;
    }

    // Records delivery order as raw addresses -- two distinct parked frames
    // are guaranteed distinct handles -- and lets tests block until a given
    // number of deliveries have happened, which is how they dodge racing the
    // timer thread.
    struct delivery_log {
        std::mutex mutex;
        std::condition_variable delivered;
        std::vector<std::uintptr_t> order;

        void record(const std::coroutine_handle<> h) {
            {
                const std::lock_guard lock(mutex);
                order.push_back(reinterpret_cast<std::uintptr_t>(h.address()));
            }
            delivered.notify_all();
        }

        void await_count(const std::size_t n) {
            std::unique_lock lock(mutex);
            delivered.wait(lock, [this, n] {
                return order.size() >= n;
            });
        }

        [[nodiscard]] std::uintptr_t at(const std::size_t i) {
            const std::lock_guard lock(mutex);
            return order[i];
        }
    };

    // Sleeps `delay` from wherever the body starts, then reports which thread
    // schedule_after put us back on.
    coro::task<std::thread::id> nap(coro::timer_scheduler& timer, const std::chrono::milliseconds delay) {
        co_await timer.schedule_after(delay);
        co_return std::this_thread::get_id();
    }

    // Sleep, then land somewhere on purpose. This is the composition the
    // standalone timer is built around: waking is the timer's job, choosing
    // where to continue is the caller's.
    coro::task<std::pair<std::thread::id, std::thread::id>> nap_then_pool(
        coro::timer_scheduler& timer, coro::static_thread_pool& pool) {
        co_await timer.schedule_after(15ms);
        const std::thread::id woke_on = std::this_thread::get_id();
        co_await pool.schedule();
        co_return std::pair{woke_on, std::this_thread::get_id()};
    }

    // schedule() with no delay attached: an unconditional hop to the timer.
    coro::task<std::thread::id> bare_hop(coro::timer_scheduler& timer) {
        co_await timer.schedule();
        co_return std::this_thread::get_id();
    }

    coro::task<int> double_nap(coro::timer_scheduler& timer) {
        co_await timer.schedule_after(15ms);
        co_await timer.schedule_after(15ms);
        co_return 2;
    }

    coro::task<int> nap_after_hop(coro::timer_scheduler& timer) {
        co_await timer.schedule();
        co_await timer.schedule_after(15ms);
        co_return 3;
    }

    coro::task<std::pair<std::thread::id, std::thread::id>> around_zero(coro::timer_scheduler& timer) {
        const std::thread::id before = std::this_thread::get_id();
        co_await timer.schedule_after(0ms);
        // Not a braced co_return: task's return_value(From&&) template cannot
        // deduce From from a brace list.
        co_return std::pair{before, std::this_thread::get_id()};
    }
}

// The queue delivers strictly in deadline order regardless of enqueue order:
// a later-submitted-but-earlier-due entry must jump the heap.
TEST(TimedWaitQueue, DeliversInDeadlineOrder) {
    delivery_log log;
    coro::detail::timed_wait_queue queue{[&log](const std::coroutine_handle<> h) {
        log.record(h);
    }};

    frame late = park();
    frame early = park();

    queue.enqueue(std::chrono::steady_clock::now() + 150ms, late.handle);
    queue.enqueue(std::chrono::steady_clock::now() + 20ms, early.handle);

    log.await_count(2);

    const auto early_address = reinterpret_cast<std::uintptr_t>(early.handle.address());
    const auto late_address = reinterpret_cast<std::uintptr_t>(late.handle.address());
    EXPECT_EQ(log.at(0), early_address);
    EXPECT_EQ(log.at(1), late_address);
}

// A deadline already in the past at enqueue time is delivered promptly --
// promptly meaning the timer thread wakes at all, with no hard latency bound.
TEST(TimedWaitQueue, PastDeadlineDelivers) {
    delivery_log log;
    coro::detail::timed_wait_queue queue{[&log](const std::coroutine_handle<> h) {
        log.record(h);
    }};

    frame done = park();
    queue.enqueue(std::chrono::steady_clock::now() - 1ms, done.handle);

    log.await_count(1);
    EXPECT_EQ(log.at(0), reinterpret_cast<std::uintptr_t>(done.handle.address()));
}

// The core contract: suspension lasts at least as long as requested. The
// timer thread only pops entries once their deadline has passed.
TEST(SleepFor, ElapsesNoEarlierThanRequested) {
    coro::timer_scheduler timer;

    constexpr auto delay = 60ms;
    const auto start = std::chrono::steady_clock::now();
    static_cast<void>(coro::sync_wait(nap(timer, delay)));
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_GE(elapsed, delay);
}

// And the non-blocking part: while the coroutine sleeps, its original thread
// was released -- proven indirectly by the wake-up landing on some OTHER
// thread than the one that started the body. That other thread is the timer's
// own, since nothing else is involved any more.
TEST(SleepFor, ResumesOnTimerThreadNotCaller) {
    coro::timer_scheduler timer;
    const auto caller = std::this_thread::get_id();
    EXPECT_NE(coro::sync_wait(nap(timer, 30ms)), caller);
}

// Two sleeps on one timer wake on the SAME thread: there is exactly one, and
// that is what makes the head-of-line warning in the header real.
TEST(SleepFor, AllSleepersShareTheOneTimerThread) {
    coro::timer_scheduler timer;
    const auto first = coro::sync_wait(nap(timer, 5ms));
    const auto second = coro::sync_wait(nap(timer, 5ms));
    EXPECT_EQ(first, second);
}

// schedule() always hops, unlike schedule_after(0) which short-circuits.
TEST(SleepFor, ScheduleHopsToTimerThread) {
    coro::timer_scheduler timer;
    const auto caller = std::this_thread::get_id();
    EXPECT_NE(coro::sync_wait(bare_hop(timer)), caller);
}

// Landing on a pool after a sleep is composition, not something the timer
// arranges: wake on the timer thread, then hop deliberately.
TEST(SleepFor, ComposesBackOntoAPool) {
    coro::static_thread_pool pool{2};
    coro::timer_scheduler timer;
    const auto caller = std::this_thread::get_id();

    const auto [woke_on, after_hop] = coro::sync_wait(nap_then_pool(timer, pool));
    EXPECT_NE(woke_on, caller) << "the sleep should not resume on the caller";
    EXPECT_NE(after_hop, woke_on) << "the pool hop should leave the timer thread";
    EXPECT_NE(after_hop, caller);
}

// Zero (and negative) delays short-circuit in await_ready: no suspension, no
// hop -- execution runs straight through on the calling thread.
TEST(SleepFor, NonPositiveDelayNeverSuspends) {
    coro::timer_scheduler timer;
    const auto [before, after] = coro::sync_wait(around_zero(timer));
    EXPECT_EQ(before, after);
}

// Ten back-to-back short sleeps through the same queue; exercises the drain,
// re-arm, and re-lock cycle of the timer thread repeatedly.
TEST(SleepFor, TenShortSleepsAllComplete) {
    coro::timer_scheduler timer;
    int completed = 0;
    for (int i = 0; i < 10; ++i) {
        static_cast<void>(coro::sync_wait(nap(timer, 2ms)));
        ++completed;
    }
    EXPECT_EQ(completed, 10);
}

// The unlocked delivery in timed_wait_queue::run() exists so a woken coroutine
// can touch the queue again without deadlocking against it. These four are the
// ways that actually happens.

// Sleeping twice: the second enqueue is issued from a resumption the timer
// itself drove.
TEST(SleepFor, ConsecutiveSleepsDoNotDeadlock) {
    coro::timer_scheduler timer;
    EXPECT_EQ(coro::sync_wait(double_nap(timer)), 2);
}

// Sleeping from a coroutine the timer itself just resumed.
TEST(SleepFor, SleepAfterHopDoesNotDeadlock) {
    coro::timer_scheduler timer;
    EXPECT_EQ(coro::sync_wait(nap_after_hop(timer)), 3);
}

// And through schedule_on -- a timer_scheduler is a scheduler, so the adaptors
// take it like any other.
TEST(SleepFor, SleepUnderScheduleOnDoesNotDeadlock) {
    coro::timer_scheduler timer;
    EXPECT_EQ(coro::sync_wait(coro::schedule_on(timer, double_nap(timer))), 2);
}

// Starting the work on a pool and sleeping from there: two independent
// schedulers in one chain, neither aware of the other.
TEST(SleepFor, SleepFromPoolWorkerDoesNotDeadlock) {
    coro::static_thread_pool pool{2};
    coro::timer_scheduler timer;
    EXPECT_EQ(coro::sync_wait(coro::schedule_on(pool, double_nap(timer))), 2);
}

// The literal self-re-entry: arming a new timer from inside a delivery
// callback, while the timer thread is mid-cycle.
TEST(TimedWaitQueue, EnqueueFromDeliveryDoesNotDeadlock) {
    std::atomic<int> fired{0};
    coro::detail::timed_wait_queue* self = nullptr;
    coro::detail::timed_wait_queue queue{[&](const std::coroutine_handle<> h) {
        if (fired.fetch_add(1) == 0) {
            self->enqueue(std::chrono::steady_clock::now() + 2ms, h);
        }
    }};
    self = &queue;

    queue.enqueue(std::chrono::steady_clock::now() + 2ms, std::noop_coroutine());
    while (fired.load() < 2) std::this_thread::sleep_for(1ms);
    EXPECT_EQ(fired.load(), 2);
}

// Several sleepers outstanding at once, so the heap holds more than one entry
// and the drain loop actually loops. Each sleeper is driven by its own OS
// thread through a separate sync_wait, so the delivery path is exercised from
// several threads at once rather than one wait_all group on a single thread.
TEST(SleepFor, ConcurrentSleepersShareOneTimerThread) {
    coro::timer_scheduler timer;

    std::vector<std::thread> drivers;
    std::atomic<int> done{0};
    for (int i = 0; i < 6; ++i) {
        drivers.emplace_back([&timer, &done, i] {
            static_cast<void>(coro::sync_wait(nap(timer, std::chrono::milliseconds{10 + 5 * i})));
            done.fetch_add(1);
        });
    }
    for (auto& t : drivers) t.join();
    EXPECT_EQ(done.load(), 6);
}

// Shutdown drains rather than abandons: entries nowhere near due are still
// handed to the callback, so their frames get resumed and can unwind instead
// of being stranded forever.
//
// Only observable in a build with asserts off. Destroying a queue with waiters
// parked IS the precondition violation, and the debug assert deliberately
// aborts on it -- which is the behaviour the other build wants.
TEST(TimedWaitQueue, ShutdownDrainsPendingWaiters) {
#ifndef NDEBUG
    GTEST_SKIP() << "debug asserts abort on the precondition violation this test relies on";
#else
    std::atomic<int> delivered{0};
    {
        coro::detail::timed_wait_queue queue{[&delivered](std::coroutine_handle<>) {
            delivered.fetch_add(1);
        }};
        queue.enqueue(std::chrono::steady_clock::now() + 1h, std::noop_coroutine());
        queue.enqueue(std::chrono::steady_clock::now() + 2h, std::noop_coroutine());
    }
    EXPECT_EQ(delivered.load(), 2) << "pending waiters were abandoned, not drained";
#endif
}
