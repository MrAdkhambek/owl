#include "rest/db.h"

#include <coro/executors/static_thread_pool.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>
#include <sql/sql.h>

#include "rest/app.h"

namespace rest {
    // A standalone pool driven with sync_wait: the same API the handlers
    // use, from plain code. The pool and its connection thread close when
    // the function returns, before the server opens its own.
    void migrate(const std::string& path) {
        coro::static_thread_pool hop{1};
        const Db db{{.path = path}, sql::scheduler_ref{hop}};
        coro::sync_wait([&]() -> coro::task<> {
            (void)co_await sql::execute<"PRAGMA journal_mode=WAL">(db);
            (void)co_await sql::execute<
                "CREATE TABLE IF NOT EXISTS users("
                "  id INTEGER PRIMARY KEY, username TEXT NOT NULL UNIQUE, password TEXT NOT NULL)">(db);
            (void)co_await sql::execute<
                "CREATE TABLE IF NOT EXISTS sessions("
                "  token TEXT PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id))">(db);
            (void)co_await sql::execute<
                "CREATE TABLE IF NOT EXISTS posts("
                "  id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL REFERENCES users(id),"
                "  title TEXT NOT NULL, body TEXT NOT NULL DEFAULT '',"
                "  created_at TEXT NOT NULL DEFAULT (datetime('now')))">(db);
        }());
    }
}
