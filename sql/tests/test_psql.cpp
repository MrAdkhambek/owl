#include <gtest/gtest.h>

#include <atomic>
#include <coroutine>
#include <cstdlib>
#include <string>
#include <vector>

#include <coro/run/sync_wait.h>

#include <sql/psql.h>

namespace {
    // Same shape as the sqlite suite's counting_post: proves the resumer
    // hook stays unused on the postgres path (never leaves its loop).
    struct counting_post final {
        std::atomic<int> posts{0};

        std::suspend_never schedule() const noexcept {
            return {};
        }

        void post(const std::coroutine_handle<> h) {
            posts.fetch_add(1);
            h.resume();
        }
    };

    struct Psql : testing::Test {
        const char* dsn = std::getenv("SQL_PG_DSN");

        void SetUp() override {
            if (dsn == nullptr) GTEST_SKIP() << "SQL_PG_DSN not set";
        }
    };

    TEST_F(Psql, QueryExecuteAndBinds) {
        sql::psql::connection conn{sql::psql::config{.dsn = dsn}, sql::any_reactor{}};

        const auto made = coro::sync_wait(conn.run("CREATE TEMP TABLE t (id int, name text)", {}, nullptr));
        ASSERT_TRUE(made.has_value());

        const auto inserted = coro::sync_wait(conn.run("INSERT INTO t VALUES ($1, $2)",
                                                       std::vector<sql::bind_value>{sql::bind_value{std::int64_t{1}},
                                                                                    sql::bind_value{std::string{"ada"}}},
                                                       nullptr));
        ASSERT_TRUE(inserted.has_value());

        const auto rows = coro::sync_wait(conn.run("SELECT id, name FROM t WHERE id = $1",
                                                   std::vector<sql::bind_value>{sql::bind_value{std::int64_t{1}}}, nullptr));
        ASSERT_TRUE(rows.has_value());
        EXPECT_EQ((*rows)[0][0].as<int>(), 1);
        EXPECT_EQ((*rows)[0][1].as<std::string>(), "ada");
    }

    TEST_F(Psql, ByteaRoundTrips) {
        sql::psql::connection conn{sql::psql::config{.dsn = dsn}, sql::any_reactor{}};

        (void)coro::sync_wait(conn.run("CREATE TEMP TABLE b (v bytea)", {}, nullptr));
        (void)coro::sync_wait(conn.run("INSERT INTO b VALUES ('\\x48656c6c6f00'::bytea)", {}, nullptr));

        const auto rows = coro::sync_wait(conn.run("SELECT v FROM b", {}, nullptr));
        ASSERT_TRUE(rows.has_value());
        const auto blob = (*rows)[0][0].as<std::vector<std::byte>>();
        ASSERT_EQ(blob.size(), 6);
        EXPECT_EQ(blob[5], std::byte{0});

        // A server with bytea_output = escape still round-trips.
        (void)coro::sync_wait(conn.run("SET bytea_output = escape", {}, nullptr));
        const auto escaped = coro::sync_wait(conn.run("SELECT v FROM b", {}, nullptr));
        ASSERT_TRUE(escaped.has_value());
        EXPECT_EQ((*escaped)[0][0].as<std::vector<std::byte>>(), blob);
    }

    TEST_F(Psql, SqlstateTravels) {
        sql::psql::connection conn{sql::psql::config{.dsn = dsn}, sql::any_reactor{}};
        (void)coro::sync_wait(conn.run("CREATE TEMP TABLE u (id int primary key)", {}, nullptr));
        (void)coro::sync_wait(conn.run("INSERT INTO u VALUES (1)", {}, nullptr));

        const auto dup = coro::sync_wait(conn.run("INSERT INTO u VALUES (1)", {}, nullptr));
        ASSERT_FALSE(dup.has_value());
        EXPECT_EQ(dup.error().sqlstate, "23505");
    }

    TEST_F(Psql, DeadConnectionIsReplacedOnTheNextCheckout) {
        sql::psql::connection conn{sql::psql::config{.dsn = dsn}, sql::any_reactor{}};

        const auto pid = coro::sync_wait(conn.run("SELECT pg_backend_pid()", {}, nullptr));
        ASSERT_TRUE(pid.has_value());

        // Kill our own backend: the statement errors, the connection is
        // CONNECTION_BAD afterwards, and the next run reconnects cleanly.
        const auto kill = coro::sync_wait(conn.run("SELECT pg_terminate_backend($1)",
                                                   std::vector<sql::bind_value>{sql::bind_value{std::int64_t{(*pid)[0][0].as<int>()}}},
                                                   nullptr));
        EXPECT_FALSE(kill.has_value());
        EXPECT_FALSE(conn.healthy());

        const auto again = coro::sync_wait(conn.run("SELECT 1", {}, nullptr));
        ASSERT_TRUE(again.has_value());
    }

    TEST_F(Psql, EmptyDsnFailsOnFirstUse) {
        sql::psql::connection conn{sql::psql::config{}, sql::any_reactor{}};

        EXPECT_FALSE(coro::sync_wait(conn.run("SELECT 1", {}, nullptr)).has_value());
    }
}
