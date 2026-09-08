#include <gtest/gtest.h>

#include <atomic>
#include <coroutine>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <coro/run/sync_wait.h>

#include <sql/execute.h>
#include <sql/query.h>
#include <sql/sqlite.h>
#include <sql/sqlite_handle.h>

namespace {
    std::shared_ptr<sql::pool<sql::sqlite>> memory_pool() {
        return std::make_shared<sql::pool<sql::sqlite>>(sql::sqlite::config{.path = ":memory:"});
    }

    TEST(Sqlite, QueryExecuteAndBinds) {
        sql::pool<sql::sqlite> pool{sql::sqlite::config{.path = ":memory:"}};

        EXPECT_TRUE(coro::sync_wait(sql::execute<"CREATE TABLE t (id INTEGER, name TEXT)">(pool)).has_value());

        const auto inserted = coro::sync_wait(sql::execute<"INSERT INTO t VALUES (?, ?)">(pool, 1, "ada"));
        ASSERT_TRUE(inserted.has_value());
        EXPECT_EQ(*inserted, 1);

        const auto rows = coro::sync_wait(sql::query<"SELECT id, name FROM t WHERE id = ?">(pool, 1));
        ASSERT_TRUE(rows.has_value());
        EXPECT_EQ(rows->size(), 1);
        EXPECT_EQ((*rows)[0][0].as<int>(), 1);
        EXPECT_EQ((*rows)[0][1].as<std::string>(), "ada");
    }

    TEST(Sqlite, TaskBuiltFromATemporaryBindsCorrectly) {
        sql::pool<sql::sqlite> pool{sql::sqlite::config{.path = ":memory:"}};
        (void)coro::sync_wait(sql::execute<"CREATE TABLE t (name TEXT)">(pool));

        auto task = sql::query<"SELECT name FROM t WHERE name = ?">(pool, std::string{"made-later"});
        const auto rows = coro::sync_wait(std::move(task));
        ASSERT_TRUE(rows.has_value());
        EXPECT_EQ(rows->size(), 0);
    }

    TEST(Sqlite, EmptyResultAndAffectedRows) {
        sql::pool<sql::sqlite> pool{sql::sqlite::config{.path = ":memory:"}};
        (void)coro::sync_wait(sql::execute<"CREATE TABLE t (v INT)">(pool));

        const auto empty = coro::sync_wait(sql::query<"SELECT v FROM t WHERE v = 1">(pool));
        ASSERT_TRUE(empty.has_value());
        EXPECT_TRUE(empty->empty());

        const auto affected = coro::sync_wait(sql::query<"INSERT INTO t VALUES (7)">(pool));
        ASSERT_TRUE(affected.has_value());
        EXPECT_TRUE(affected->empty());
        EXPECT_TRUE(affected->has_rows_affected());
        EXPECT_EQ(affected->rows_affected(), 1);
    }

    TEST(Sqlite, NullCarriesThrough) {
        sql::pool<sql::sqlite> pool{sql::sqlite::config{.path = ":memory:"}};
        (void)coro::sync_wait(sql::execute<"CREATE TABLE t (v TEXT)">(pool));
        (void)coro::sync_wait(sql::execute<"INSERT INTO t VALUES (?)">(pool, nullptr));

        const auto rows = coro::sync_wait(sql::query<"SELECT v FROM t">(pool));
        ASSERT_TRUE(rows.has_value());
        EXPECT_EQ((*rows)[0][0].optional<std::string>(), std::nullopt);
    }

    TEST(Sqlite, BarePoolResumesInlineOnTheReactorThread) {
        sql::pool<sql::sqlite> pool{sql::sqlite::config{.path = ":memory:"}};

        auto body = [&]() -> coro::task<void> {
            const auto before = std::this_thread::get_id();
            const auto r = co_await sql::query<"SELECT 1">(pool);
            EXPECT_TRUE(r.has_value());
            // Empty resumer: the reactor thread resumed the coroutine
            // inline, so the continuation is no longer on the caller.
            EXPECT_NE(std::this_thread::get_id(), before);
        };
        coro::sync_wait(body());
    }

    TEST(Sqlite, HandleResumesThroughItsResumer) {
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

        counting_post post;
        auto pool = memory_pool();
        const sql::sqlite_handle db{pool, sql::resumer{&post}};

        auto body = [&]() -> coro::task<void> {
            const auto before = std::this_thread::get_id();
            const auto r = co_await sql::query<"SELECT 42 AS answer">(db);
            EXPECT_TRUE(r.has_value());
            EXPECT_EQ((*r)[0][0].as<int>(), 42);
            // The completion went through post: on the posting thread
            // (the reactor thread here -- under owl it is the worker loop).
            // Two hops: the lazy connect and the statement itself.
            EXPECT_NE(std::this_thread::get_id(), before);
            EXPECT_GE(post.posts.load(), 1);
        };
        coro::sync_wait(body());
    }
}
