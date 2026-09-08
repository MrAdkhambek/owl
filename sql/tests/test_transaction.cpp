#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include <coro/run/sync_wait.h>

#include <sql/execute.h>
#include <sql/pool.h>
#include <sql/query.h>
#include <sql/sqlite.h>
#include <sql/transaction.h>

namespace {
    TEST(Transaction, CommitPersists) {
        sql::pool<sql::sqlite> pool{sql::sqlite::config{.path = ":memory:"}};
        (void)coro::sync_wait(sql::execute<"CREATE TABLE t (v INT)">(pool));

        const auto r = coro::sync_wait(sql::transaction(pool, [](auto tx) -> coro::task<std::expected<int, sql::error>> {
            (void)co_await sql::execute<"INSERT INTO t VALUES (1)">(tx);
            // Queries in the body see the uncommitted row -- same
            // connection.
            const auto probe = co_await sql::query<"SELECT COUNT(*) FROM t">(tx);
            if (!probe) co_return std::unexpected(probe.error());
            co_return (*probe)[0][0].template as<int>();
        }));
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(*r, 1);

        const auto after = coro::sync_wait(sql::query<"SELECT COUNT(*) FROM t">(pool));
        ASSERT_TRUE(after.has_value());
        EXPECT_EQ((*after)[0][0].as<int>(), 1);
    }

    TEST(Transaction, RollbackOnUnexpected) {
        sql::pool<sql::sqlite> pool{sql::sqlite::config{.path = ":memory:"}};
        (void)coro::sync_wait(sql::execute<"CREATE TABLE t (v INT)">(pool));

        const auto r = coro::sync_wait(sql::transaction(pool, [](auto tx) -> coro::task<std::expected<int, sql::error>> {
            (void)co_await sql::execute<"INSERT INTO t VALUES (1)">(tx);
            co_return std::unexpected(sql::error{.message = "nope"});
        }));
        EXPECT_FALSE(r.has_value());
        EXPECT_EQ(r.error().message, "nope");

        const auto after = coro::sync_wait(sql::query<"SELECT COUNT(*) FROM t">(pool));
        ASSERT_TRUE(after.has_value());
        EXPECT_EQ((*after)[0][0].as<int>(), 0);
    }

    TEST(Transaction, RollbackAndRethrowOnThrow) {
        sql::pool<sql::sqlite> pool{sql::sqlite::config{.path = ":memory:"}};
        (void)coro::sync_wait(sql::execute<"CREATE TABLE t (v INT)">(pool));

        const auto threw = coro::sync_wait([&]() -> coro::task<bool> {
            try {
                (void)co_await sql::transaction(pool, [](auto tx) -> coro::task<std::expected<int, sql::error>> {
                    (void)co_await sql::execute<"INSERT INTO t VALUES (1)">(tx);
                    throw std::runtime_error("programmer error");
                    co_return 0;
                });
            } catch (const std::runtime_error& e) {
                co_return std::string{e.what()} == "programmer error";
            }
            co_return false;
        }());
        EXPECT_TRUE(threw);

        const auto after = coro::sync_wait(sql::query<"SELECT COUNT(*) FROM t">(pool));
        ASSERT_TRUE(after.has_value());
        EXPECT_EQ((*after)[0][0].as<int>(), 0);
    }
}
