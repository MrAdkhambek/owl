#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include <libpq-fe.h>

#include <coro/io/reactor.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <sql/error.h>
#include <sql/pool.h>
#include <sql/psql.h>
#include <sql/query.h>
#include <sql/transaction.h>

using namespace std::chrono_literals;

namespace {
    const char* dsn() {
        return std::getenv("OWL_TEST_PSQL_DSN");
    }

    struct fixture final {
        coro::native_reactor reactor;
        sql::reactor_ref io{reactor};
        sql::pool<sql::psql> pg;

        explicit fixture(sql::psql::config cfg) : pg(std::move(cfg), io) {
        }
    };

    sql::psql::config live(const std::chrono::milliseconds query_timeout = -1ms) {
        return {.dsn = dsn(), .query_timeout = query_timeout};
    }
}

#define OWL_REQUIRE_PSQL() \
    if (dsn() == nullptr) GTEST_SKIP() << "OWL_TEST_PSQL_DSN is not set"

TEST(Psql, IsADriver) {
    static_assert(sql::driver<sql::psql>);
    static_assert(sql::result_like<sql::psql::result>);
}

TEST(Psql, RefusedConnectionIsConnect) {
    fixture fx{{.dsn = "postgres://127.0.0.1:1/nope", .connect_timeout = 2000ms}};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await sql::try_query<"SELECT 1">(fx.pg);
        EXPECT_EQ(r.error().kind(), sql::error_kind::connect);
        EXPECT_EQ(fx.pg.size(), 0u);
    }());
}

TEST(Psql, NullReactorCannotConnect) {
    sql::pool<sql::psql> pg{{.dsn = "postgres://127.0.0.1:1/nope"}, sql::reactor_ref{}};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await sql::try_query<"SELECT 1">(pg);
        EXPECT_EQ(r.error().kind(), sql::error_kind::connect);
    }());
}

TEST(Psql, RoundTripsEveryBindableType) {
    OWL_REQUIRE_PSQL();
    fixture fx{live()};
    const sql::blob bytes{std::byte{0}, std::byte{0xab}, std::byte{0xff}};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await sql::query<"SELECT $1::int, $2::float8, $3::text, $4::bytea, $5::bool, $6::int, $7::text, $8::bigint">(
            fx.pg, 7, 2.5, std::string{"hi"}, bytes, true, std::nullopt, "lit", std::optional<std::int64_t>{-9007199254740993});
        EXPECT_EQ(r.rows(), 1u);
        EXPECT_EQ(r.columns(), 8u);
        EXPECT_EQ(r.affected(), 0u);
        EXPECT_EQ(r[0][0].as<int>(), 7);
        EXPECT_DOUBLE_EQ(r[0][1].as<double>(), 2.5);
        EXPECT_EQ(r[0][2].as<std::string>(), "hi");
        EXPECT_EQ(r[0][2].as<std::string_view>(), "hi");
        EXPECT_EQ(r[0][3].as<sql::blob>(), bytes);
        EXPECT_TRUE(r[0][4].as<bool>());
        EXPECT_TRUE(r[0][5].is_null());
        EXPECT_EQ(r[0][5].as<std::optional<int>>(), std::nullopt);
        EXPECT_THROW((void)r[0][5].as<int>(), sql::error);
        EXPECT_EQ(r[0][6].as<std::string>(), "lit");
        EXPECT_EQ(r[0][7].as<std::int64_t>(), std::int64_t{-9007199254740993});
        EXPECT_NE(r.raw(), nullptr);
        EXPECT_EQ(fx.pg.idle(), 1u);
    }());
}

TEST(Psql, ExecuteReportsAffectedRowsAndNamedColumnsResolve) {
    OWL_REQUIRE_PSQL();
    fixture fx{live()};
    coro::sync_wait([&]() -> coro::task<> {
        EXPECT_EQ(co_await sql::execute<"CREATE TEMP TABLE t(i int)">(fx.pg), 0u);
        EXPECT_EQ(co_await sql::execute<"INSERT INTO t SELECT generate_series(1, 3)">(fx.pg), 3u);
        EXPECT_EQ(co_await sql::execute<"UPDATE t SET i = i WHERE i > $1">(fx.pg, 1), 2u);
        const auto r = co_await sql::query<"SELECT count(*) AS n, min(i) AS lo FROM t">(fx.pg);
        EXPECT_EQ(r[0]["n"].as<int>(), 3);
        EXPECT_EQ(r[0]["lo"].as<int>(), 1);
        EXPECT_EQ((r[0].as<int, int>()), (std::tuple<int, int>{3, 1}));
        EXPECT_THROW((void)r[0]["zzz"], sql::error);
        EXPECT_THROW((void)r[0].as<int>(), sql::error);
        int seen = 0;
        for (const auto row : r) seen += row[0].as<int>();
        EXPECT_EQ(seen, 3);
    }());
}

TEST(Psql, QueryErrorsCarrySqlstateAndKeepTheConnection) {
    OWL_REQUIRE_PSQL();
    fixture fx{live()};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await sql::try_query<"SELECT * FROM nope_table">(fx.pg);
        EXPECT_EQ(r.error().kind(), sql::error_kind::query);
        EXPECT_EQ(r.error().sqlstate(), "42P01");
        EXPECT_EQ(r.error().code(), static_cast<int>(PGRES_FATAL_ERROR));
        EXPECT_EQ(fx.pg.size(), 1u);
        EXPECT_EQ(fx.pg.idle(), 1u);
        const auto again = co_await sql::query<"SELECT 1">(fx.pg);
        EXPECT_EQ(again[0][0].as<int>(), 1);
        EXPECT_EQ(fx.pg.size(), 1u);
    }());
}

TEST(Psql, QueryTimeoutFinishesTheConnectionAndThePoolReplacesIt) {
    OWL_REQUIRE_PSQL();
    fixture fx{live(200ms)};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await sql::try_query<"SELECT pg_sleep(2)">(fx.pg);
        EXPECT_EQ(r.error().kind(), sql::error_kind::timeout);
        EXPECT_EQ(fx.pg.size(), 0u);
        const auto again = co_await sql::query<"SELECT 2">(fx.pg);
        EXPECT_EQ(again[0][0].as<int>(), 2);
        EXPECT_EQ(fx.pg.size(), 1u);
    }());
}

TEST(Psql, TerminatedBackendIsConnectionAndTheTransactionAbandonsIt) {
    OWL_REQUIRE_PSQL();
    fixture fx{live()};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await sql::transaction(fx.pg, [](auto tx) -> coro::task<std::expected<int, sql::error>> {
            const auto killed = co_await sql::try_query<"SELECT pg_terminate_backend(pg_backend_pid())">(tx);
            EXPECT_EQ(killed.error().kind(), sql::error_kind::connection);
            co_return std::unexpected(killed.error());
        });
        EXPECT_EQ(r.error().kind(), sql::error_kind::connection);
        EXPECT_EQ(fx.pg.size(), 0u);
        const auto again = co_await sql::query<"SELECT 3">(fx.pg);
        EXPECT_EQ(again[0][0].as<int>(), 3);
    }());
}

TEST(Psql, TransactionsCommitAndRollBack) {
    OWL_REQUIRE_PSQL();
    fixture fx{live()};
    coro::sync_wait([&]() -> coro::task<> {
        (void)co_await sql::execute<"CREATE TEMP TABLE t(i int)">(fx.pg);
        const auto rolled = co_await sql::transaction(fx.pg, [](auto tx) -> coro::task<std::expected<int, sql::error>> {
            (void)co_await sql::execute<"INSERT INTO t VALUES (1)">(tx);
            co_return std::unexpected(sql::error{sql::error_kind::query, "changed my mind"});
        });
        EXPECT_FALSE(rolled.has_value());
        const auto committed = co_await sql::transaction(fx.pg, [](auto tx) -> coro::task<std::expected<int, sql::error>> {
            (void)co_await sql::execute<"INSERT INTO t VALUES (2)">(tx);
            const auto r = co_await sql::query<"SELECT count(*) FROM t">(tx);
            co_return r[0][0].template as<int>();
        });
        EXPECT_EQ(committed.value(), 1);
        const auto r = co_await sql::query<"SELECT i FROM t">(fx.pg);
        EXPECT_EQ(r.rows(), 1u);
        EXPECT_EQ(r[0][0].as<int>(), 2);
        EXPECT_EQ(fx.pg.size(), 1u);
    }());
}

TEST(Psql, ConnectionsOpenUpToTheLimitAndAreReused) {
    OWL_REQUIRE_PSQL();
    fixture fx{{.dsn = dsn(), .connections = 2}};
    coro::sync_wait([&]() -> coro::task<> {
        const auto a = co_await fx.pg.checkout();
        const auto b = co_await fx.pg.checkout();
        EXPECT_TRUE(a.has_value() && b.has_value());
        EXPECT_EQ(fx.pg.size(), 2u);
        EXPECT_EQ(fx.pg.idle(), 0u);
    }());
    EXPECT_EQ(fx.pg.idle(), 2u);
    coro::sync_wait([&]() -> coro::task<> {
        (void)co_await sql::query<"SELECT 1">(fx.pg);
        EXPECT_EQ(fx.pg.size(), 2u);
    }());
}
