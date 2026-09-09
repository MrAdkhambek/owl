#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include <coro/io/deadline.h>

using namespace std::chrono_literals;

TEST(Deadline, DefaultIsUnbounded) {
    const coro::deadline d;
    EXPECT_FALSE(d.bounded());
    EXPECT_LT(d.remaining().count(), 0);
    std::this_thread::sleep_for(2ms);
    EXPECT_LT(d.remaining().count(), 0);
}

TEST(Deadline, NegativeBudgetIsUnbounded) {
    const coro::deadline d{-1ms};
    EXPECT_FALSE(d.bounded());
    EXPECT_LT(d.remaining().count(), 0);
}

TEST(Deadline, BoundedCountsDown) {
    const coro::deadline d{200ms};
    EXPECT_TRUE(d.bounded());
    const auto first = d.remaining();
    EXPECT_GT(first.count(), 0);
    EXPECT_LE(first.count(), 200);
    std::this_thread::sleep_for(20ms);
    EXPECT_LT(d.remaining().count(), first.count());
}

TEST(Deadline, LapsedAsksForAPollNotAnInfiniteWait) {
    const coro::deadline d{1ms};
    std::this_thread::sleep_for(5ms);
    EXPECT_EQ(d.remaining().count(), 0);
}

// Rounding up, so a fraction of a millisecond left is still a wait rather
// than an immediate timeout.
TEST(Deadline, ZeroBudgetIsBoundedAndAlreadyLapsed) {
    const coro::deadline d{0ms};
    EXPECT_TRUE(d.bounded());
    EXPECT_EQ(d.remaining().count(), 0);
}
