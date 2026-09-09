#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <gtest/gtest.h>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

#include <coro/algo/when_any.h>
#include <coro/executors/static_thread_pool.h>
#include <coro/executors/timer_scheduler.h>
#include <coro/run/schedule_on.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

using namespace std::chrono_literals;

namespace {
    coro::task<int> value(const int n) { co_return n; }

    coro::task<> nothing() { co_return; }

    coro::task<int> counted(const int n, std::atomic<int>& finished) {
        finished.fetch_add(1);
        co_return n;
    }

    // Spins until `flag` reaches `target`, for a bounded time. Losers finish
    // on their own schedule, on their own thread; tests that let them lose
    // wait for them here before tearing down the executor they parked on.
    void wait_for(const std::atomic<int>& flag, const int target) {
        for (int i = 0; i < 400 && flag.load() != target; ++i) std::this_thread::sleep_for(5ms);
    }
}

// Every test owns a timer, and every loser it starts parks on that timer.
// TearDown waits for those losers: when_any has no cancellation, so a loser
// keeps running after the race is decided, and the timer must outlive it.
class when_any_t : public testing::Test {
protected:
    coro::timer_scheduler timer;
    std::atomic<int> losers_parked{0};

    // Loses to anything that is already ready: parks for a while first.
    coro::task<> slow() {
        losers_parked.fetch_add(1);
        co_await timer.schedule_after(20ms);
        losers_parked.fetch_sub(1);
    }

    void TearDown() override {
        wait_for(losers_parked, 0);
        EXPECT_EQ(losers_parked.load(), 0) << "a loser never completed";
    }
};

// The first child that is already ready wins, and the variant index says
// which one it was. when_any starts children in argument order, so a ready
// task ahead of a slow one always reports index 0.
TEST_F(when_any_t, ReportsTheWinnerByIndex) {
    auto body = [this]() -> coro::task<std::size_t> {
        auto winner = co_await coro::when_any(value(7), slow());
        co_return winner.index();
    };

    EXPECT_EQ(coro::sync_wait(body()), 0u);
}

TEST_F(when_any_t, CarriesTheWinningValue) {
    auto body = [this]() -> coro::task<int> {
        auto winner = co_await coro::when_any(value(7), slow());
        co_return std::get<0>(winner);
    };

    EXPECT_EQ(coro::sync_wait(body()), 7);
}

// Two void children are still distinguishable, because variant indices are
// positional -- this is why void_value exists instead of std::monostate.
TEST_F(when_any_t, TwoVoidChildrenStayDistinguishable) {
    auto body = [this]() -> coro::task<std::size_t> {
        auto winner = co_await coro::when_any(slow(), nothing());
        co_return winner.index();
    };

    EXPECT_EQ(coro::sync_wait(body()), 1u);
}

TEST_F(when_any_t, LosersDoNotDelayTheResult) {
    auto body = [this]() -> coro::task<> {
        co_await coro::when_any(slow(), value(1));
    };

    const auto start = std::chrono::steady_clock::now();
    coro::sync_wait(body());
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, 15ms);
}

// The README's timeout idiom, from the loser's side: the child parked on
// the timer is still there when when_any returns and the group is gone. It
// has to finish on its own, into memory that outlives the group -- not be
// freed underneath the timer thread that still holds its handle.
TEST_F(when_any_t, LosingChildRunsToCompletionAfterTheGroupIsGone) {
    auto body = [this]() -> coro::task<std::size_t> {
        auto winner = co_await coro::when_any(slow(), value(7));
        co_return winner.index();
    };

    EXPECT_EQ(coro::sync_wait(body()), 1u);
    EXPECT_EQ(losers_parked.load(), 1) << "the loser finished before it could have";
    wait_for(losers_parked, 0);
    EXPECT_EQ(losers_parked.load(), 0) << "the loser never completed";
}

// Children finishing on pool workers race each other for the win, and race
// the parent's own check for who resumes it. The parent must be resumed
// exactly once with a child that actually finished, and every loser must
// still run to its end on the pool rather than be pulled out from under it.
TEST_F(when_any_t, ChildrenCompletingOnPoolWorkersResumeTheParentOnce) {
    coro::static_thread_pool pool{4};
    std::atomic<int> finished{0};

    auto body = [&]() -> coro::task<int> {
        int total = 0;
        for (int i = 0; i < 200; ++i) {
            auto winner = co_await coro::when_any(
                coro::schedule_on(pool, counted(1, finished)),
                coro::schedule_on(pool, counted(2, finished)));
            total += std::visit([](const int v) { return v; }, winner);
        }
        co_return total;
    };

    const int total = coro::sync_wait(body());
    EXPECT_GE(total, 200);
    EXPECT_LE(total, 400);

    wait_for(finished, 400);
    EXPECT_EQ(finished.load(), 400) << "losers did not run to completion";
}

// An empty range can never produce a winner, so it throws rather than
// waiting forever.
TEST_F(when_any_t, EmptyRangeThrows) {
    auto body = []() -> coro::task<> {
        std::vector<coro::task<int>> none;
        co_await coro::when_any(std::move(none));
    };

    EXPECT_THROW(coro::sync_wait(body()), std::invalid_argument);
}

// The range form reports the winner's position, and its losers -- all
// parked on the timer here -- finish after the group is gone, as above.
TEST_F(when_any_t, RangeReportsTheEarliestToFinish) {
    std::atomic<int> finished{0};
    auto nap = [&](const std::chrono::milliseconds delay) -> coro::task<> {
        co_await timer.schedule_after(delay);
        finished.fetch_add(1);
    };

    auto body = [&]() -> coro::task<std::size_t> {
        std::vector<coro::task<>> children;
        children.push_back(nap(30ms));
        children.push_back(nap(10ms));
        children.push_back(nap(20ms));
        auto [index, token] = co_await coro::when_any(std::move(children));
        static_assert(std::is_same_v<decltype(token), coro::void_value>);
        co_return index;
    };

    EXPECT_EQ(coro::sync_wait(body()), 1u);
    wait_for(finished, 3);
    EXPECT_EQ(finished.load(), 3) << "range losers did not run to completion";
}
