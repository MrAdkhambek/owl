#include <chrono>
#include <coroutine>
#include <gtest/gtest.h>
#include <vector>

// The whole point of this file: coro.h is the library's advertised entry
// point, and nothing else in the suite includes it. A broken include path in
// there is invisible to every other target -- it shipped exactly that way
// once, with ctest fully green -- so this TU exists to make the umbrella a
// build-time dependency of the test suite.
#include <coro/coro.h>
#include "support/helpers.h"

using namespace std::chrono_literals;
using namespace coro_test;

namespace {
    // The concept lattice, asserted on concrete types. The headers cannot do
    // this themselves: scheduler.h must not know about pools, and
    // timer_scheduler.h is a template with no instantiation of its own.
    static_assert(coro::scheduler<coro::static_thread_pool>);
    static_assert(coro::postable<coro::static_thread_pool>);
    static_assert(!coro::delayed_scheduler<coro::static_thread_pool>,
                  "timing belongs to timer_scheduler, not the pool");

    // Standalone: a timer_scheduler is a scheduler in its own right, needing
    // no other one to wrap.
    static_assert(coro::scheduler<coro::timer_scheduler>);
    static_assert(coro::delayed_scheduler<coro::timer_scheduler>);

    // A scheduler with nowhere to put a bare handle: it satisfies `scheduler`
    // and nothing more, which is exactly why `postable` is opt-in rather than
    // folded into `scheduler`.
    struct inline_scheduler {
        std::suspend_never schedule() const noexcept {
            return {};
        }
    };

    static_assert(coro::scheduler<inline_scheduler>);
    static_assert(!coro::postable<inline_scheduler>);
    static_assert(!coro::delayed_scheduler<inline_scheduler>);

    coro::async_generator<int> counted(const int n) {
        for (int i = 1; i <= n; ++i) co_yield i;
    }

    // One touch of every header the umbrella pulls in: task, async_generator,
    // the pool, the timer, both scheduler adaptors, async_mutex,
    // fmap, result, as_result, wait_all, sync_wait.
    coro::task<int> everything(coro::timer_scheduler& timer,
                               coro::static_thread_pool& pool,
                               coro::async_mutex& gate) {
        co_await pool.schedule();
        co_await timer.schedule_after(1ms);
        co_await pool.schedule();

        int total = 0;
        auto squares = counted(4) | coro::fmap([](const int x) { return x * x; });
        while (auto v = co_await squares.next()) {
            const auto guard = co_await gate.scoped_lock();
            total += *v;
        }

        co_return total;
    }
}

TEST(Umbrella, EverythingCompilesAndComposes) {
    reset_started();
    coro::static_thread_pool pool{2};
    coro::timer_scheduler timer;
    coro::async_mutex gate;

    auto [a, b] = coro::sync_wait(coro::wait_all(
        everything(timer, pool, gate),
        coro::as_result(coro::resume_on(pool, val(2) | coro::fmap([](const int x) { return x * 10; })))));

    EXPECT_EQ(a, 1 + 4 + 9 + 16);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(*b, 20);
    EXPECT_EQ(start_count(), 1);
}
