#include <gtest/gtest.h>

#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <coro/task.h>

#include <sql/error.h>
#include <sql/pool.h>

#include "support/fake_driver.h"

namespace {
    using fake = sql_test::fake;
    using pool_t = sql::pool<fake>;
    using lease_t = sql::lease<fake>;

    struct fixture final {
        fake::script s;
        pool_t pool;

        explicit fixture(const unsigned connections) : pool({.connections = connections, .script_ = &s}, {}) {
        }
    };

    // Starts a checkout and hands the lease out through `slot` when it lands.
    coro::task<> take(const pool_t& pool, std::optional<lease_t>& slot, std::optional<sql::error>& failure) {
        auto r = co_await pool.checkout();
        if (r) slot.emplace(std::move(*r));
        else failure.emplace(std::move(r.error()));
    }

    coro::task<> run_one(const pool_t& pool, int& done) {
        auto l = co_await pool.checkout();
        EXPECT_TRUE(l.has_value());
        if (!l) co_return;
        const auto r = co_await (*l)->execute<"X">();
        EXPECT_TRUE(r.has_value());
        ++done;
    }
}

TEST(Pool, SatisfiesSource) {
    static_assert(sql::source<pool_t>);
}

TEST(Pool, IdleConnectionIsReusedWithoutReopening) {
    fixture fx{1};
    std::optional<lease_t> a;
    std::optional<sql::error> err;
    auto t1 = take(fx.pool, a, err);
    t1.start();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(fx.pool.size(), 1u);
    EXPECT_EQ(fx.pool.idle(), 0u);
    a.reset();
    EXPECT_EQ(fx.pool.idle(), 1u);

    std::optional<lease_t> b;
    auto t2 = take(fx.pool, b, err);
    t2.start();
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(fx.s.opened, 1);
    b.reset();
}

TEST(Pool, OpensLazilyUpToTheLimitThenParks) {
    fixture fx{2};
    std::optional<lease_t> a, b, c;
    std::optional<sql::error> err;
    auto t1 = take(fx.pool, a, err);
    auto t2 = take(fx.pool, b, err);
    auto t3 = take(fx.pool, c, err);
    t1.start();
    t2.start();
    t3.start();
    EXPECT_TRUE(a && b);
    EXPECT_FALSE(c);
    EXPECT_EQ(fx.s.opened, 2);
    EXPECT_EQ(fx.pool.waiting(), 1u);

    a.reset();
    EXPECT_TRUE(c);
    EXPECT_EQ(fx.s.opened, 2);
    EXPECT_EQ(fx.pool.waiting(), 0u);
    EXPECT_EQ(fx.pool.idle(), 0u);
    b.reset();
    c.reset();
}

TEST(Pool, WaitersAreServedFifoByDirectHandoff) {
    fixture fx{1};
    std::optional<lease_t> a, b, c;
    std::optional<sql::error> err;
    auto t1 = take(fx.pool, a, err);
    auto t2 = take(fx.pool, b, err);
    auto t3 = take(fx.pool, c, err);
    t1.start();
    t2.start();
    t3.start();
    ASSERT_TRUE(a);
    EXPECT_EQ(fx.pool.waiting(), 2u);

    a.reset();
    EXPECT_TRUE(b);
    EXPECT_FALSE(c);
    b.reset();
    EXPECT_TRUE(c);
    EXPECT_EQ(fx.pool.idle(), 0u);
    c.reset();
    EXPECT_EQ(fx.pool.idle(), 1u);
    EXPECT_EQ(fx.s.opened, 1);
}

TEST(Pool, BrokenConnectionIsDestroyedOnReleaseAndReplacedLazily) {
    fixture fx{1};
    fx.s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::connection, "gone"}));
    auto body = [&]() -> coro::task<> {
        auto l = co_await fx.pool.checkout();
        EXPECT_EQ((co_await (*l)->execute<"X">()).error().kind(), sql::error_kind::connection);
    };
    auto t = body();
    t.start();
    EXPECT_EQ(fx.s.destroyed, 1);
    EXPECT_EQ(fx.pool.size(), 0u);

    int done = 0;
    auto t2 = run_one(fx.pool, done);
    t2.start();
    EXPECT_EQ(done, 1);
    EXPECT_EQ(fx.s.opened, 2);
}

TEST(Pool, BrokenReleaseWakesAWaiterToOpenAFreshOne) {
    fixture fx{1};
    fx.s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::connection, "gone"}));
    std::optional<lease_t> b;
    std::optional<sql::error> err;
    auto body = [&]() -> coro::task<> {
        auto l = co_await fx.pool.checkout();
        (void)co_await (*l)->execute<"X">();
        fx.s.suspend = true;
        co_await (*l)->execute<"Y">();
        fx.s.suspend = false;
    };
    auto t1 = body();
    t1.start();
    auto t2 = take(fx.pool, b, err);
    t2.start();
    EXPECT_EQ(fx.pool.waiting(), 1u);
    fx.s.resume_all();
    ASSERT_TRUE(b);
    EXPECT_EQ(fx.s.opened, 2);
    EXPECT_EQ(fx.s.destroyed, 1);
    b.reset();
}

TEST(Pool, OpenFailureIsReportedAndNotCached) {
    fixture fx{1};
    fx.s.opens.emplace_back(std::unexpected(sql::error{sql::error_kind::connect, "refused"}));
    std::optional<lease_t> a;
    std::optional<sql::error> err;
    auto t1 = take(fx.pool, a, err);
    t1.start();
    ASSERT_TRUE(err);
    EXPECT_EQ(err->kind(), sql::error_kind::connect);
    EXPECT_EQ(fx.pool.size(), 0u);

    err.reset();
    auto t2 = take(fx.pool, a, err);
    t2.start();
    EXPECT_TRUE(a);
    EXPECT_EQ(fx.s.opened, 2);
    a.reset();
}

TEST(Pool, OpenFailureWakesAParkedWaiter) {
    fixture fx{1};
    fx.s.suspend_open = true;
    fx.s.opens.emplace_back(std::unexpected(sql::error{sql::error_kind::connect, "refused"}));
    std::optional<lease_t> a, b;
    std::optional<sql::error> err_a, err_b;
    auto t1 = take(fx.pool, a, err_a);
    auto t2 = take(fx.pool, b, err_b);
    t1.start();
    t2.start();
    EXPECT_EQ(fx.pool.waiting(), 1u);
    fx.s.suspend_open = false;
    fx.s.resume_all();
    EXPECT_TRUE(err_a);
    EXPECT_TRUE(b);
    EXPECT_EQ(fx.s.opened, 2);
    b.reset();
}

TEST(Pool, CloseFailsWaitersDestroysIdleAndRefusesCheckout) {
    fixture fx{1};
    std::optional<lease_t> a, b, c;
    std::optional<sql::error> err_b, err_c;
    auto t1 = take(fx.pool, a, err_b);
    auto t2 = take(fx.pool, b, err_b);
    t1.start();
    t2.start();
    fx.pool.close();
    ASSERT_TRUE(err_b);
    EXPECT_EQ(err_b->kind(), sql::error_kind::closed);
    a.reset();
    EXPECT_EQ(fx.s.destroyed, 1);
    EXPECT_EQ(fx.pool.size(), 0u);
    auto t3 = take(fx.pool, c, err_c);
    t3.start();
    EXPECT_EQ(err_c->kind(), sql::error_kind::closed);
}

TEST(Pool, DrainLoopServesAChainOfImmediateCompletionsWithoutRecursion) {
    fixture fx{1};
    fx.s.suspend = true;
    int done = 0;
    std::vector<coro::task<>> tasks;
    for (int i = 0; i < 5000; ++i) tasks.push_back(run_one(fx.pool, done));
    for (auto& t : tasks) t.start();
    EXPECT_EQ(done, 0);
    EXPECT_EQ(fx.pool.waiting(), 4999u);
    fx.s.suspend = false;
    fx.s.resume_all();
    EXPECT_EQ(done, 5000);
    EXPECT_EQ(fx.s.opened, 1);
    EXPECT_EQ(fx.s.log.size(), 5000u);
}

TEST(Pool, LeaseMoveTransfersOwnership) {
    fixture fx{1};
    std::optional<lease_t> a;
    std::optional<sql::error> err;
    auto t = take(fx.pool, a, err);
    t.start();
    lease_t moved = std::move(*a);
    a.reset();
    EXPECT_EQ(fx.pool.idle(), 0u);
    EXPECT_TRUE(moved);
    moved = lease_t{};
    EXPECT_EQ(fx.pool.idle(), 1u);
}
