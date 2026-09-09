#include <chrono>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <tuple>

#include <coro/executors/timer_scheduler.h>
#include <coro/result/as_tuple.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

using namespace std::chrono_literals;

namespace {
    coro::task<int> value(const int n) { co_return n; }

    coro::task<int> boom() {
        throw std::runtime_error("boom");
        co_return 0;
    }

    coro::task<> nothing() { co_return; }
}

TEST(as_tuple_t, SuccessLeavesTheErrorSlotNull) {
    auto body = []() -> coro::task<int> {
        auto [err, n] = co_await coro::as_tuple(value(41));
        EXPECT_EQ(err, nullptr);
        co_return n + 1;
    };

    EXPECT_EQ(coro::sync_wait(body()), 42);
}

// On failure the value slot is value-initialised rather than left
// indeterminate, which is why as_tuple requires a default-constructible T.
TEST(as_tuple_t, FailureFillsTheErrorSlotAndZeroesTheValue) {
    auto body = []() -> coro::task<bool> {
        auto [err, n] = co_await coro::as_tuple(boom());
        EXPECT_EQ(n, 0);
        co_return err != nullptr;
    };

    EXPECT_TRUE(coro::sync_wait(body()));
}

TEST(as_tuple_t, VoidChildProducesAOneElementTuple) {
    auto body = []() -> coro::task<bool> {
        auto tup = co_await coro::as_tuple(nothing());
        static_assert(std::tuple_size_v<decltype(tup)> == 1);
        co_return std::get<0>(tup) == nullptr;
    };

    EXPECT_TRUE(coro::sync_wait(body()));
}

TEST(as_tuple_t, ErrorSlotCarriesTheOriginalException) {
    auto body = []() -> coro::task<std::string> {
        auto [err, n] = co_await coro::as_tuple(boom());
        try {
            std::rethrow_exception(err);
        } catch (const std::runtime_error& e) {
            co_return e.what();
        }
    };

    EXPECT_EQ(coro::sync_wait(body()), "boom");
}

// An adapter named before it is awaited: the timer awaiter it wrapped was a
// temporary that died at the end of that statement, so the adapter must hold
// its own copy of a movable child rather than a reference into a dead one.
TEST(as_tuple_t, NamedAdapterOwnsItsRvalueAwaiter) {
    coro::timer_scheduler timer;

    auto body = [&timer]() -> coro::task<bool> {
        auto adapted = coro::as_tuple(timer.schedule_after(1ms));
        auto [err] = co_await adapted;
        co_return err == nullptr;
    };

    EXPECT_TRUE(coro::sync_wait(body()));
}
