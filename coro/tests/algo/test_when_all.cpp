#include <chrono>
#include <gtest/gtest.h>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <vector>

#include <coro/algo/when_all.h>
#include <coro/executors/timer_scheduler.h>
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
