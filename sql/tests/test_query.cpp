#include <gtest/gtest.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include <coro/task.h>

#include <sql/error.h>
#include <sql/pool.h>
#include <sql/query.h>

#include "support/fake_driver.h"

namespace {
    using fake = sql_test::fake;
    using pool_t = sql::pool<fake>;

    struct fixture final {
        fake::script s;
        pool_t pool{{.connections = 1, .script_ = &s}, {}};
    };

    void drive(coro::task<>&& t) {
        t.start();
        EXPECT_TRUE(t.done());
    }
}

TEST(Query, TypesLineUpWithTheDriver) {
    static_assert(std::is_same_v<sql::result_t<pool_t>, fake::result>);
    static_assert(std::is_same_v<decltype(sql::try_query<"SELECT 1">(std::declval<const pool_t&>())),
                                 coro::task<std::expected<fake::result, sql::error>>>);
    static_assert(std::is_same_v<decltype(sql::query<"SELECT 1">(std::declval<const pool_t&>())),
                                 coro::task<fake::result>>);
    static_assert(std::is_same_v<decltype(sql::try_execute<"SELECT 1">(std::declval<const pool_t&>())),
                                 coro::task<std::expected<std::uint64_t, sql::error>>>);
    static_assert(std::is_same_v<decltype(sql::execute<"SELECT 1">(std::declval<const pool_t&>())),
                                 coro::task<std::uint64_t>>);
}

TEST(Query, TryQueryChecksOutRunsAndReleasesBeforeReturning) {
    fixture fx;
    fx.s.rows.push_back({{"7"}});
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::try_query<"SELECT $1">(fx.pool, 7);
        EXPECT_TRUE(r.has_value());
        EXPECT_EQ(fx.pool.idle(), 1u);
        EXPECT_EQ((*r)[0][0].as<int>(), 7);
        EXPECT_EQ(fx.s.log, (std::vector<std::string>{"SELECT $1"}));
    }());
}

TEST(Query, TryQueryReportsCheckoutFailure) {
    fixture fx;
    fx.s.opens.emplace_back(std::unexpected(sql::error{sql::error_kind::connect, "refused"}));
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::try_query<"SELECT 1">(fx.pool);
        EXPECT_FALSE(r.has_value());
        EXPECT_EQ(r.error().kind(), sql::error_kind::connect);
        EXPECT_TRUE(fx.s.log.empty());
    }());
}

TEST(Query, QueryThrowsTheDriversError) {
    fixture fx;
    fx.s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::query, "syntax", "42601"}));
    drive([&]() -> coro::task<> {
        bool thrown = false;
        try {
            (void)co_await sql::query<"SELECT 1">(fx.pool);
        } catch (const sql::error& e) {
            thrown = e.kind() == sql::error_kind::query && e.sqlstate() == "42601";
        }
        EXPECT_TRUE(thrown);
        EXPECT_EQ(fx.pool.idle(), 1u);
    }());
}

TEST(Query, TryExecuteMapsToAffected) {
    fixture fx;
    fx.s.runs.emplace_back(std::uint64_t{3});
    drive([&]() -> coro::task<> {
        const auto n = co_await sql::try_execute<"DELETE FROM t WHERE a = $1 AND b = $2">(fx.pool, 1, std::string{"x"});
        EXPECT_EQ(n.value(), 3u);
    }());
}

TEST(Query, ExecuteThrowsAndReturnsTheCount) {
    fixture fx;
    fx.s.runs.emplace_back(std::uint64_t{2});
    fx.s.runs.emplace_back(std::unexpected(sql::error{sql::error_kind::connection, "gone"}));
    drive([&]() -> coro::task<> {
        EXPECT_EQ(co_await sql::execute<"UPDATE t SET a = $1">(fx.pool, std::optional<int>{}), 2u);
        bool thrown = false;
        try {
            (void)co_await sql::execute<"UPDATE t SET a = $1">(fx.pool, 1);
        } catch (const sql::error& e) {
            thrown = e.kind() == sql::error_kind::connection;
        }
        EXPECT_TRUE(thrown);
        EXPECT_EQ(fx.pool.size(), 0u);
    }());
}

TEST(Query, EveryBindableSpellingIsAccepted) {
    fixture fx;
    const std::string owned = "s";
    const sql::blob bytes{std::byte{1}};
    drive([&]() -> coro::task<> {
        const auto r = co_await sql::try_query<"SELECT $1, $2, $3, $4, $5, $6, $7, $8">(
            fx.pool, true, 1, 2.5, owned, "lit", bytes, std::nullopt, std::optional<double>{1.0});
        EXPECT_TRUE(r.has_value());
    }());
}
