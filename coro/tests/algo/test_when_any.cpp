#include <chrono>
#include <coroutine>
#include <cstddef>
#include <gtest/gtest.h>
#include <stdexcept>
#include <variant>
#include <vector>

#include <coro/algo/when_any.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

using namespace std::chrono_literals;

namespace {
    coro::task<int> value(const int n) { co_return n; }

    coro::task<> nothing() { co_return; }

    // Parks forever. Destroying its leaf is safe: nothing outside when_any
    // holds the handle, unlike a timer_scheduler waiter.
    coro::task<> never() { co_await std::suspend_always{}; }
}

// The first child that is already ready wins, and the variant index says
// which one it was. when_any starts children in argument order, so a pair
// of ready tasks always reports index 0.
TEST(when_any_t, ReportsTheWinnerByIndex) {
    auto body = []() -> coro::task<std::size_t> {
        auto winner = co_await coro::when_any(value(7), never());
        co_return winner.index();
    };

    EXPECT_EQ(coro::sync_wait(body()), 0u);
}

TEST(when_any_t, CarriesTheWinningValue) {
    auto body = []() -> coro::task<int> {
        auto winner = co_await coro::when_any(value(7), never());
        co_return std::get<0>(winner);
    };

    EXPECT_EQ(coro::sync_wait(body()), 7);
}

// Two void children are still distinguishable, because variant indices are
// positional -- this is why void_value exists instead of std::monostate.
TEST(when_any_t, TwoVoidChildrenStayDistinguishable) {
    auto body = []() -> coro::task<std::size_t> {
        auto winner = co_await coro::when_any(never(), nothing());
        co_return winner.index();
    };

    EXPECT_EQ(coro::sync_wait(body()), 1u);
}

TEST(when_any_t, LosersDoNotDelayTheResult) {
    auto body = []() -> coro::task<> {
        co_await coro::when_any(never(), value(1));
    };

    const auto start = std::chrono::steady_clock::now();
    coro::sync_wait(body());
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, 200ms);
}

// An empty range can never produce a winner, so it throws rather than
// waiting forever.
TEST(when_any_t, EmptyRangeThrows) {
    auto body = []() -> coro::task<> {
        std::vector<coro::task<int>> none;
        co_await coro::when_any(std::move(none));
    };

    EXPECT_THROW(coro::sync_wait(body()), std::invalid_argument);
}
