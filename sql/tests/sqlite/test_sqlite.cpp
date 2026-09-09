#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <tuple>

#include <sqlite3.h>

#include <coro/executors/static_thread_pool.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <sql/error.h>
#include <sql/pool.h>
#include <sql/query.h>
#include <sql/sqlite.h>
#include <sql/transaction.h>

#include "support/temp_db.h"

using namespace std::chrono_literals;

namespace {
    using sql_test::temp_db;

    struct fixture final {
        temp_db file;
        coro::static_thread_pool hop{1};
        sql::scheduler_ref io{hop};
        sql::pool<sql::sqlite> db;

        explicit fixture(const std::string& name, const unsigned connections = 1)
            : file(name), db({.path = file.path.string(), .connections = connections}, io) {
        }
    };

    coro::task<> create_t(const sql::pool<sql::sqlite>& db) {
        (void)co_await sql::execute<"CREATE TABLE t(i INTEGER, f REAL, s TEXT, b BLOB, n)">(db);
    }
}

TEST(Sqlite, IsADriver) {
    static_assert(sql::driver<sql::sqlite>);
    static_assert(sql::result_like<sql::sqlite::result>);
}

TEST(Sqlite, RoundTripsEveryStorageClass) {
    fixture fx{"roundtrip"};
    const sql::blob bytes{std::byte{0}, std::byte{0xab}};
    coro::sync_wait([&]() -> coro::task<> {
        co_await create_t(fx.db);
        const auto n = co_await sql::execute<"INSERT INTO t VALUES ($1, $2, $3, $4, $5)">(
            fx.db, std::int64_t{-5}, 2.5, std::string{"hi"}, bytes, std::nullopt);
        EXPECT_EQ(n, 1u);
        const auto r = co_await sql::query<"SELECT i, f, s, b, n FROM t">(fx.db);
        EXPECT_EQ(r.rows(), 1u);
        EXPECT_EQ(r.columns(), 5u);
        EXPECT_EQ(r.affected(), 0u);
        EXPECT_EQ(r[0][0].as<std::int64_t>(), -5);
        EXPECT_DOUBLE_EQ(r[0][1].as<double>(), 2.5);
        EXPECT_EQ(r[0][2].as<std::string>(), "hi");
        EXPECT_EQ(r[0][2].as<std::string_view>(), "hi");
        EXPECT_EQ(r[0][3].as<sql::blob>(), bytes);
        EXPECT_TRUE(r[0][4].is_null());
        EXPECT_EQ(r[0][4].as<std::optional<int>>(), std::nullopt);
        EXPECT_EQ(r[0]["s"].as<std::string>(), "hi");
        EXPECT_EQ((r[0].as<std::int64_t, double, std::string, sql::blob, std::optional<int>>()),
                  (std::tuple<std::int64_t, double, std::string, sql::blob, std::optional<int>>{-5, 2.5, "hi", bytes, std::nullopt}));
    }());
}

TEST(Sqlite, StorageClassesArePreservedAndConversionsAreStrict) {
    fixture fx{"classes"};
    coro::sync_wait([&]() -> coro::task<> {
        co_await create_t(fx.db);
        (void)co_await sql::execute<"INSERT INTO t(i, f, s, b, n) VALUES (300, 1.5, '42', x'00', 1)">(fx.db);
        const auto r = co_await sql::query<"SELECT i, f, s, b, n FROM t">(fx.db);
        EXPECT_EQ(r[0][0].as<int>(), 300);
        EXPECT_DOUBLE_EQ(r[0][0].as<double>(), 300.0);
        EXPECT_TRUE(r[0][0].as<bool>());
        EXPECT_THROW((void)r[0][0].as<std::int8_t>(), sql::error);
        EXPECT_THROW((void)r[0][0].as<std::string>(), sql::error);
        EXPECT_EQ(r[0][2].as<int>(), 42);
        EXPECT_EQ(r[0][2].as<std::string>(), "42");
        EXPECT_THROW((void)r[0][1].as<int>(), sql::error);
        EXPECT_THROW((void)r[0][3].as<std::string>(), sql::error);
        EXPECT_TRUE(r[0][4].as<bool>());
        EXPECT_THROW((void)r[0][4].as<std::string>(), sql::error);
    }());
}

TEST(Sqlite, PlaceholderNumberWinsOverAppearanceOrder) {
    fixture fx{"order"};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await sql::query<"SELECT $2, $1">(fx.db, 1, 2);
        EXPECT_EQ((r[0].as<int, int>()), (std::tuple<int, int>{2, 1}));
    }());
}

TEST(Sqlite, StatementsAreCachedPerLiteral) {
    fixture fx{"cache"};
    coro::sync_wait([&]() -> coro::task<> {
        (void)co_await sql::query<"SELECT 1">(fx.db);
        (void)co_await sql::query<"SELECT 1">(fx.db);
        (void)co_await sql::query<"SELECT 2">(fx.db);
        const auto l = co_await fx.db.checkout();
        EXPECT_EQ((*l)->cached_statements(), 2u);
    }());
}

TEST(Sqlite, QueryErrorsCarryTheCodeAndKeepTheConnection) {
    fixture fx{"errors"};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await sql::try_query<"SELECT * FROM nope">(fx.db);
        EXPECT_EQ(r.error().kind(), sql::error_kind::query);
        EXPECT_EQ(r.error().code(), SQLITE_ERROR);
        EXPECT_NE(std::string{r.error().what()}.find("nope"), std::string::npos);
        EXPECT_EQ(fx.db.size(), 1u);
        EXPECT_EQ(fx.db.idle(), 1u);
    }());
}

TEST(Sqlite, OpenFailureIsConnect) {
    coro::static_thread_pool hop{1};
    sql::scheduler_ref io{hop};
    sql::pool<sql::sqlite> db{{.path = "/nonexistent-dir-owl/x.db"}, io};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await sql::try_query<"SELECT 1">(db);
        EXPECT_EQ(r.error().kind(), sql::error_kind::connect);
        EXPECT_EQ(r.error().code() & 0xff, SQLITE_CANTOPEN);
        EXPECT_EQ(db.size(), 0u);
    }());
}

TEST(Sqlite, BusyTimeoutSurfacesAsSqliteBusy) {
    fixture writer{"busy"};
    coro::static_thread_pool hop{1};
    sql::scheduler_ref io{hop};
    sql::pool<sql::sqlite> reader{{.path = writer.file.path.string(), .busy_timeout = 100ms}, io};
    coro::sync_wait([&]() -> coro::task<> {
        co_await create_t(writer.db);
        const auto held = co_await writer.db.checkout();
        EXPECT_TRUE((co_await (*held)->execute<"BEGIN IMMEDIATE">()).has_value());
        const auto r = co_await sql::try_execute<"INSERT INTO t(i) VALUES (1)">(reader);
        EXPECT_EQ(r.error().kind(), sql::error_kind::query);
        EXPECT_EQ(r.error().code() & 0xff, SQLITE_BUSY);
        EXPECT_TRUE((co_await (*held)->execute<"ROLLBACK">()).has_value());
    }());
}

TEST(Sqlite, AffectedIsZeroForReadOnlyStatements) {
    fixture fx{"affected"};
    coro::sync_wait([&]() -> coro::task<> {
        co_await create_t(fx.db);
        EXPECT_EQ(co_await sql::execute<"INSERT INTO t(i) VALUES (1), (2)">(fx.db), 2u);
        EXPECT_EQ(co_await sql::execute<"SELECT i FROM t">(fx.db), 0u);
        EXPECT_EQ(co_await sql::execute<"DELETE FROM t WHERE i = $1">(fx.db, 1), 1u);
    }());
}

TEST(Sqlite, RowAsTupleRequiresTheExactWidth) {
    fixture fx{"width"};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await sql::query<"SELECT 1, 2">(fx.db);
        EXPECT_THROW((void)r[0].as<int>(), sql::error);
        EXPECT_THROW((void)(r[0].as<int, int, int>()), sql::error);
        EXPECT_THROW((void)r[1], sql::error);
        EXPECT_THROW((void)r[0][2], sql::error);
        EXPECT_THROW((void)r[0]["zzz"], sql::error);
        int seen = 0;
        for (const auto row : r) seen += row[1].as<int>();
        EXPECT_EQ(seen, 2);
    }());
}

TEST(Sqlite, CompletionsResumeOnTheScheduler) {
    fixture fx{"resume"};
    coro::sync_wait([&]() -> coro::task<> {
        co_await fx.io.schedule();
        const auto hop_id = std::this_thread::get_id();
        (void)co_await sql::try_query<"SELECT 1">(fx.db);
        EXPECT_EQ(std::this_thread::get_id(), hop_id);
    }());
}

TEST(Sqlite, TransactionsCommitAndRollBack) {
    fixture fx{"tx"};
    coro::sync_wait([&]() -> coro::task<> {
        co_await create_t(fx.db);
        const auto rolled = co_await sql::transaction(fx.db, [](auto tx) -> coro::task<std::expected<int, sql::error>> {
            (void)co_await sql::execute<"INSERT INTO t(i) VALUES (1)">(tx);
            co_return std::unexpected(sql::error{sql::error_kind::query, "changed my mind"});
        });
        EXPECT_FALSE(rolled.has_value());
        const auto committed = co_await sql::transaction(fx.db, [](auto tx) -> coro::task<std::expected<int, sql::error>> {
            (void)co_await sql::execute<"INSERT INTO t(i) VALUES (2)">(tx);
            const auto r = co_await sql::query<"SELECT count(*) FROM t">(tx);
            co_return r[0][0].template as<int>();
        });
        EXPECT_EQ(committed.value(), 1);
        const auto r = co_await sql::query<"SELECT i FROM t">(fx.db);
        EXPECT_EQ(r.rows(), 1u);
        EXPECT_EQ(r[0][0].as<int>(), 2);
    }());
}
