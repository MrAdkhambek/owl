#include "rest/db.h"

#include <coro/io/reactor.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>
#include <sql/sql.h>

#include "rest/app.h"

namespace rest {
    // A standalone pool driven with sync_wait: the same API the handlers
    // use, from plain code. The reactor is declared first so it outlives
    // the pool, and both close when the function returns, before the
    // server opens its own.
    void migrate(const sql::psql::config& cfg) {
        coro::native_reactor reactor;
        const Db db{cfg, sql::reactor_ref{reactor}};
        coro::sync_wait([&]() -> coro::task<> {
            (void)co_await sql::execute<
                "CREATE TABLE IF NOT EXISTS users("
                "  id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"
                "  username TEXT NOT NULL UNIQUE, password TEXT NOT NULL)">(db);
            // Sessions moved to redis, where they expire on their own; a
            // database from before that still carries the table.
            (void)co_await sql::execute<"DROP TABLE IF EXISTS sessions">(db);
            (void)co_await sql::execute<
                "CREATE TABLE IF NOT EXISTS posts("
                "  id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,"
                "  user_id BIGINT NOT NULL REFERENCES users(id),"
                "  title TEXT NOT NULL, body TEXT NOT NULL DEFAULT '',"
                "  created_at TIMESTAMPTZ NOT NULL DEFAULT now())">(db);
        }());
    }
}
