#include <gtest/gtest.h>

#include <expected>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <coro/task.h>

#include <sql/error.h>
#include <sql/pool.h>
#include <sql/query.h>
#include <sql/transaction.h>

#include "support/fake_driver.h"

namespace {
    using fake = sql_test::fake;
    using pool_t = sql::pool<fake>;
    using tx_t = sql::transaction_source<fake>;
    using outcome = std::expected<int, sql::error>;

    struct fixture final {
        fake::script s;
        pool_t pool{{.connections = 1, .script_ = &s}, {}};
    };

    void drive(coro::task<>&& t) {
        t.start();
        EXPECT_TRUE(t.done());
    }

    const std::vector<std::string> begin_commit{"BEGIN", "SELECT 1", "COMMIT"};
    const std::vector<std::string> begin_rollback{"BEGIN", "SELECT 1", "ROLLBACK"};
}

TEST(TransactionBody, AcceptsEverySpellingOfTheSourceParameter) {
    const auto by_value = [](auto tx) -> coro::task<outcome> { (void)tx; co_return 1; };
    const auto by_ref = [](auto& tx) -> coro::task<outcome> { (void)tx; co_return 1; };
    const auto by_cref = [](const auto& tx) -> coro::task<outcome> { (void)tx; co_return 1; };
    const auto typed = [](tx_t& tx) -> coro::task<std::expected<std::string, sql::error>> { (void)tx; co_return "x"; };
    static_assert(sql::transaction_body<decltype(by_value), fake>);
    static_assert(sql::transaction_body<decltype(by_ref), fake>);
    static_assert(sql::transaction_body<decltype(by_cref), fake>);
    static_assert(sql::transaction_body<decltype(typed), fake>);
    static_assert(std::is_same_v<sql::transaction_value_t<decltype(by_value), fake>, int>);
    static_assert(std::is_same_v<sql::transaction_value_t<decltype(typed), fake>, std::string>);
    static_assert(sql::source<tx_t>);
    static_assert(std::is_copy_constructible_v<tx_t>);
}

TEST(TransactionBody, RejectsOtherShapes) {
    const auto plain_task = [](tx_t&) -> coro::task<int> { co_return 1; };
    const auto not_a_task = [](tx_t&) -> outcome { return 1; };
    const auto wrong_arg = [](int) -> coro::task<outcome> { co_return 1; };
    static_assert(!sql::transaction_body<decltype(plain_task), fake>);
    static_assert(!sql::transaction_body<decltype(not_a_task), fake>);
    static_assert(!sql::transaction_body<decltype(wrong_arg), fake>);
}

TEST(Transaction, CommitsOnAValue) {
    fixture fx;
    fx.s.rows.push_back({});
    fx.s.rows.push_back({{"42"}});
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::transaction(fx.pool, [&](auto tx) -> coro::task<outcome> {
            EXPECT_EQ(fx.pool.idle(), 0u);
            const auto q = co_await sql::try_query<"SELECT 1">(tx);
            if (!q) co_return std::unexpected(q.error());
            co_return (*q)[0][0].template as<int>();
        });
        EXPECT_EQ(r.value(), 42);
        EXPECT_EQ(fx.s.log, begin_commit);
        EXPECT_EQ(fx.s.opened, 1);
        EXPECT_EQ(fx.pool.idle(), 1u);
    }());
}

TEST(Transaction, RollsBackOnABodyError) {
    fixture fx;
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::transaction(fx.pool, [](auto& tx) -> coro::task<outcome> {
            (void)co_await sql::try_query<"SELECT 1">(tx);
            co_return std::unexpected(sql::error{sql::error_kind::query, "nope"});
        });
        EXPECT_EQ(r.error().kind(), sql::error_kind::query);
        EXPECT_STREQ(r.error().what(), "nope");
        EXPECT_EQ(fx.s.log, begin_rollback);
        EXPECT_EQ(fx.pool.idle(), 1u);
        EXPECT_EQ(fx.s.destroyed, 0);
    }());
}

TEST(Transaction, RollbackFailureLosesToTheBodyErrorAndAbandons) {
    fixture fx;
    fx.s.runs.emplace_back(std::uint64_t{0});   // BEGIN
    fx.s.runs.emplace_back(std::uint64_t{0});   // SELECT 1
    fx.s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::query, "rollback failed"}));
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::transaction(fx.pool, [](auto tx) -> coro::task<outcome> {
            (void)co_await sql::try_query<"SELECT 1">(tx);
            co_return std::unexpected(sql::error{sql::error_kind::query, "body"});
        });
        EXPECT_STREQ(r.error().what(), "body");
        EXPECT_EQ(fx.s.log, begin_rollback);
        EXPECT_EQ(fx.s.destroyed, 1);
        EXPECT_EQ(fx.pool.size(), 0u);
    }());
}

TEST(Transaction, BeginFailureIsReportedAndTheBodyNeverRuns) {
    fixture fx;
    fx.s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::query, "no begin"}));
    drive([&]() -> coro::task<> {
        bool ran = false;
        const auto r = co_await sql::transaction(fx.pool, [&](auto) -> coro::task<outcome> {
            ran = true;
            co_return 1;
        });
        EXPECT_STREQ(r.error().what(), "no begin");
        EXPECT_FALSE(ran);
        EXPECT_EQ(fx.s.log, (std::vector<std::string>{"BEGIN"}));
    }());
}

TEST(Transaction, CommitFailureIsReported) {
    fixture fx;
    fx.s.runs.emplace_back(std::uint64_t{0});
    fx.s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::query, "no commit"}));
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::transaction(fx.pool, [](auto) -> coro::task<outcome> { co_return 1; });
        EXPECT_STREQ(r.error().what(), "no commit");
        EXPECT_EQ(fx.s.log, (std::vector<std::string>{"BEGIN", "COMMIT"}));
    }());
}

TEST(Transaction, ThrowingBodyRollsBackRethrowsAndKeepsTheConnection) {
    fixture fx;
    drive([&]() -> coro::task<> {
        bool thrown = false;
        try {
            (void)co_await sql::transaction(fx.pool, [](auto tx) -> coro::task<outcome> {
                (void)co_await sql::try_query<"SELECT 1">(tx);
                throw std::runtime_error("boom");
                co_return 1;
            });
        } catch (const std::runtime_error& e) {
            thrown = std::string{e.what()} == "boom";
        }
        EXPECT_TRUE(thrown);
        EXPECT_EQ(fx.s.log, begin_rollback);
        EXPECT_EQ(fx.pool.idle(), 1u);
        EXPECT_EQ(fx.s.destroyed, 0);
    }());
}

TEST(Transaction, RollbackFailureAfterAThrowAbandons) {
    fixture fx;
    fx.s.runs.emplace_back(std::uint64_t{0});
    fx.s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::query, "rollback failed"}));
    drive([&]() -> coro::task<> {
        bool thrown = false;
        try {
            (void)co_await sql::transaction(fx.pool, [](auto) -> coro::task<outcome> {
                throw std::runtime_error("boom");
                co_return 1;
            });
        } catch (const std::runtime_error&) {
            thrown = true;
        }
        EXPECT_TRUE(thrown);
        EXPECT_EQ(fx.s.destroyed, 1);
        EXPECT_EQ(fx.pool.size(), 0u);
    }());
}

TEST(Transaction, VoidBody) {
    fixture fx;
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::transaction(fx.pool, [](auto tx) -> coro::task<std::expected<void, sql::error>> {
            co_return (co_await sql::try_execute<"DELETE FROM t">(tx)).transform([](auto) {});
        });
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(fx.s.log, (std::vector<std::string>{"BEGIN", "DELETE FROM t", "COMMIT"}));
    }());
}

TEST(Transaction, CheckoutFailurePropagates) {
    fixture fx;
    fx.s.opens.emplace_back(std::unexpected(sql::error{sql::error_kind::connect, "refused"}));
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::transaction(fx.pool, [](auto) -> coro::task<outcome> { co_return 1; });
        EXPECT_EQ(r.error().kind(), sql::error_kind::connect);
        EXPECT_TRUE(fx.s.log.empty());
    }());
}
