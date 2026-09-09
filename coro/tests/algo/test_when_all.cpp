#include <chrono>
#include <gtest/gtest.h>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <vector>

#include <coro/algo/when_all.h>
#include <coro/executors/static_thread_pool.h>
#include <coro/executors/timer_scheduler.h>
#include <coro/run/schedule_on.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

using namespace std::chrono_literals;

namespace {
    coro::task<int> value(const int n) { co_return n; }

    coro::task<> nothing() { co_return; }

    coro::task<int> boom() {
        throw std::runtime_error("boom");
        co_return 0;
    }
}

TEST(when_all_t, ReturnsATupleInArgumentOrder) {
    auto body = []() -> coro::task<std::tuple<int, int>> {
        co_return co_await coro::when_all(value(1), value(2));
    };

    auto [a, b] = coro::sync_wait(body());
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 2);
}

// Void children still occupy a slot, so positions stay meaningful.
TEST(when_all_t, VoidChildrenOccupyAVoidSlot) {
    auto body = []() -> coro::task<int> {
        auto [a, v, b] = co_await coro::when_all(value(1), nothing(), value(3));
        static_assert(std::is_same_v<decltype(v), coro::void_value>);
        co_return a + b;
    };

    EXPECT_EQ(coro::sync_wait(body()), 4);
}

TEST(when_all_t, RethrowsAChildFailure) {
    auto body = []() -> coro::task<> {
        co_await coro::when_all(value(1), boom());
    };

    EXPECT_THROW(coro::sync_wait(body()), std::runtime_error);
}

// Children run concurrently rather than in sequence: two 40ms sleeps
// together must cost far less than 80ms.
TEST(when_all_t, ChildrenOverlap) {
    coro::timer_scheduler timer;

    auto body = [&timer]() -> coro::task<> {
        co_await coro::when_all(timer.schedule_after(40ms), timer.schedule_after(40ms));
    };

    const auto start = std::chrono::steady_clock::now();
    coro::sync_wait(body());
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, 70ms);
    EXPECT_GE(elapsed, 35ms);
}

TEST(when_all_t, RangeOverloadWaitsForEvery) {
    auto body = []() -> coro::task<int> {
        std::vector<coro::task<int>> children;
        for (int i = 1; i <= 4; ++i) children.push_back(value(i));

        auto results = co_await coro::when_all(std::move(children));
        int total = 0;
        for (const int r : results) total += r;
        co_return total;
    };

    EXPECT_EQ(coro::sync_wait(body()), 10);
}

TEST(when_all_t, EmptyPackIsReadyImmediately) {
    auto body = []() -> coro::task<int> {
        co_await coro::when_all();
        co_return 99;
    };

    EXPECT_EQ(coro::sync_wait(body()), 99);
}

// Children finishing on pool workers race each other, and race the parent's
// own arrival, on the count that decides who resumes the parent. Two workers
// that both read 2 and both write 1 leave a count that never reaches zero,
// and the group hangs; two that both see zero resume the parent twice.
TEST(when_all_t, ChildrenCompletingOnPoolWorkersAreCountedOnce) {
    coro::static_thread_pool pool{4};

    auto body = [&pool]() -> coro::task<int> {
        int total = 0;
        for (int i = 0; i < 200; ++i) {
            auto [a, b, c, d] = co_await coro::when_all(
                coro::schedule_on(pool, value(1)), coro::schedule_on(pool, value(2)),
                coro::schedule_on(pool, value(3)), coro::schedule_on(pool, value(4)));
            total += a + b + c + d;
        }
        co_return total;
    };

    EXPECT_EQ(coro::sync_wait(body()), 200 * 10);
}

// A group named before it is awaited: the task temporaries it was built from
// are gone by the time the co_await runs, so the group must have taken them
// over at construction rather than borrowed them by reference.
TEST(when_all_t, NamedGroupOwnsItsRvalueChildren) {
    auto body = []() -> coro::task<int> {
        auto group = coro::when_all(value(1), value(2));
        auto [a, b] = co_await group;
        co_return a + b;
    };

    EXPECT_EQ(coro::sync_wait(body()), 3);
}
