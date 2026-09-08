#include <gtest/gtest.h>

#include <memory>
#include <thread>

#include <coro/algo/when_all.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <sql/sqlite.h>
#include <sql/sqlite_handle.h>

namespace {
    TEST(Pool, QueuedWaiterWakesOnRelease) {
        sql::pool<sql::sqlite> pool{sql::sqlite::config{.path = ":memory:"}};

        auto held = coro::sync_wait(pool.acquire());
        ASSERT_TRUE(held.has_value());

        // Release from another thread, the way a sqlite release arrives
        // from the reactor thread: the queued waiter must wake through its
        // own resumer and complete.
        std::thread releaser([&held] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            held->release();
        });

        auto body = [&]() -> coro::task<bool> {
            auto c = co_await pool.acquire();  // queued: the only slot is held
            if (!c) co_return false;
            const auto r = co_await c->run("SELECT 1", {});
            c->release();
            co_return r.has_value();
        };
        EXPECT_TRUE(coro::sync_wait(body()));
        releaser.join();
    }

    TEST(Pool, CloseFailsPendingCheckouts) {
        auto pool = std::make_shared<sql::pool<sql::sqlite>>(sql::sqlite::config{.path = ":memory:"});

        auto held = coro::sync_wait(pool->acquire());
        EXPECT_TRUE(held.has_value());

        pool->close();
        EXPECT_FALSE(coro::sync_wait(pool->acquire()).has_value());
    }

    TEST(Pool, DrainThenCheckoutFails) {
        auto pool = std::make_shared<sql::pool<sql::sqlite>>(sql::sqlite::config{.path = ":memory:"});

        coro::sync_wait(pool->drain());
        EXPECT_FALSE(coro::sync_wait(pool->acquire()).has_value());
    }

    TEST(Pool, DestructorWithInFlightWork) {
        auto pool = std::make_shared<sql::pool<sql::sqlite>>(sql::sqlite::config{.path = ":memory:"});

        // An in-flight statement then release, then destruction: the dtor
        // blocks until leased_ reaches zero and does not hang.
        const auto done = coro::sync_wait([&]() -> coro::task<bool> {
            auto c = co_await pool->acquire();
            if (!c) co_return false;
            const auto made = co_await c->run("CREATE TABLE t (v INT)", {});
            if (!made) co_return false;
            const auto r = co_await c->run("INSERT INTO t VALUES (1)", {});
            c->release();
            co_return r.has_value();
        }());
        EXPECT_TRUE(done);
        pool.reset();
    }

    TEST(SqliteHandle, NullHandleFailsAcquireNotCrash) {
        const sql::sqlite_handle db;

        EXPECT_FALSE(static_cast<bool>(db));
        EXPECT_FALSE(coro::sync_wait(db.acquire()).has_value());
    }

    TEST(SqliteHandle, BarePoolAndHandleBothCheckOut) {
        auto pool = std::make_shared<sql::pool<sql::sqlite>>(sql::sqlite::config{.path = ":memory:"});
        const sql::sqlite_handle db{pool, sql::resumer{}};

        EXPECT_TRUE(coro::sync_wait(db.acquire()).has_value());
    }
}
