#pragma once

// Shared state, the way axum's State<T> carries it: one instance for the
// whole server, handed to handlers as owl::State<App>. The sqlite pool and
// the redis client are not here -- owl gives every worker its own,
// extracted as const Db& and const Cache&.

#include <coro/executors/static_thread_pool.h>
#include <redis/redis.h>
#include <sql/sql.h>

namespace rest {
    using Db = sql::pool<sql::psql>;
    // Sessions and login counters, each with a TTL redis enforces for us.
    using Cache = redis::client;

    struct App {
        // PBKDF2 takes tens of milliseconds by design. It runs here, never
        // on a worker's loop.
        coro::static_thread_pool hashing{2};
    };
}
