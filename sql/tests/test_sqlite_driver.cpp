#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <coro/run/sync_wait.h>

#include <sql/sqlite.h>

namespace {
    TEST(SqliteDriver, ConnectQueryAndExecute) {
        sql::sqlite::reactor r;
        sql::sqlite::connection conn{sql::sqlite::config{.path = ":memory:"}, r};

        EXPECT_TRUE(coro::sync_wait(conn.connect(nullptr)).has_value());

        const auto made = coro::sync_wait(conn.run("CREATE TABLE t (v TEXT)", {}, nullptr));
        ASSERT_TRUE(made.has_value());

        const auto inserted = coro::sync_wait(
            conn.run("INSERT INTO t VALUES (?)", std::vector<sql::bind_value>{sql::bind_value{std::string{"ada"}}}, nullptr));
        ASSERT_TRUE(inserted.has_value());
        EXPECT_EQ(inserted->rows_affected(), 1);

        const auto rows = coro::sync_wait(conn.run("SELECT v FROM t", {}, nullptr));
        ASSERT_TRUE(rows.has_value());
        EXPECT_EQ((*rows)[0][0].as<std::string>(), "ada");
    }

    TEST(SqliteDriver, NullAndErrorsAreData) {
        sql::sqlite::reactor r;
        sql::sqlite::connection conn{sql::sqlite::config{.path = ":memory:"}, r};
        (void)coro::sync_wait(conn.connect(nullptr));
        (void)coro::sync_wait(conn.run("CREATE TABLE t (v INT)", {}, nullptr));

        const auto rows = coro::sync_wait(conn.run("SELECT NULL AS n", {}, nullptr));
        ASSERT_TRUE(rows.has_value());
        EXPECT_TRUE((*rows)[0][0].is_null());

        const auto bad = coro::sync_wait(conn.run("SELECT * FROM missing", {}, nullptr));
        EXPECT_FALSE(bad.has_value());
        EXPECT_NE(bad.error().code, 0);
    }

    TEST(SqliteDriver, OneStatementPerCall) {
        sql::sqlite::reactor r;
        sql::sqlite::connection conn{sql::sqlite::config{.path = ":memory:"}, r};
        (void)coro::sync_wait(conn.connect(nullptr));

        const auto two = coro::sync_wait(conn.run("SELECT 1; SELECT 2", {}, nullptr));
        EXPECT_FALSE(two.has_value());
    }

    TEST(SqliteDriver, BadPathFailsConnect) {
        sql::sqlite::reactor r;
        sql::sqlite::connection conn{sql::sqlite::config{.path = "/nonexistent-dir-xyz/app.db"}, r};

        EXPECT_FALSE(coro::sync_wait(conn.connect(nullptr)).has_value());
    }
}
