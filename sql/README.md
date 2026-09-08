# sql

Non-blocking SQL for owl: `co_await sql::query<"…">(src, args…)` against sqlite and
PostgreSQL, with connection pools and callback transactions. SQL failure is data
(`std::expected`); conversion and programmer errors throw.

- Link `owl::sql`. Pulls `owl::coro`, `owl::fstr`, and whichever drivers are enabled.
  Enable drivers at configure time — `cmake -DOWN_ENABLE_SQLITE=ON -DOWN_ENABLE_POSTGRESQL=ON`
  — or link a single tier (`owl::sql_core`, `owl::sql_sqlite`, `owl::sql_psql`).
- `#include <sql/sql.h>` — every public header; driver headers are gated on the
  `OWL_ENABLE_*` macros.
- Sources: `sql::pool<sql::sqlite>` (process-wide: one reactor thread, one connection,
  WAL, every statement in the process serialises through it), `sql::pool<sql::psql>`
  (per-worker connections driven on the caller's event loop), and `sql::sqlite_handle`
  (the pool plus a resumer, what an owl worker holds).

```c++
const auto r = co_await sql::query<"QUERY">(pool);
const auto r = co_await sql::query<"SELECT * FROM users WHERE id = $1">(pool, user_id);

const auto n = co_await sql::execute<"INSERT INTO users(name) VALUES (?)">(pool, name);

const auto tx_r = co_await sql::transaction(pool, [](auto tx) -> coro::task<std::expected<int, sql::error>> {
    const auto r = co_await sql::query<"QUERY">(tx);
    if (!r) co_return std::unexpected(r.error());
    co_return (*r)[0][0].as<int>();
});
```

Results are taopq-shaped: `result` owns a driver-neutral table, `row` and `column` are
views into it. `.as<T>()` for builtins (arithmetic, `bool`, strings, blobs),
`std::optional<T>` maps SQL NULL, tuples/pairs consume N columns left to right. Cells
are bytes with a length — a blob containing `0x00` survives.

Placeholders are checked at compile time from the statement NTTP: `?` is the sqlite
style, `$1..$n` the postgres style, never mixed, no named (`:x`, `@x`, `$x`) or numbered
(`?5`) parameters, no holes (`$1` and `$3` without `$2`). A statement with no bind site
fits either dialect. There is no quote-awareness, so a literal `?` or `$` inside a string
constant is uncompilable — lift it out and pass it as a bind (`SELECT ?` with `"what?"`).
Postgres casts (`::`) are not bind sites and are fine.

Under owl:

```c++
owl::Server<App>::builder()
    .router(...)
    .with_psql(sql::psql::config{.dsn = "postgres://localhost/app"})
    .with_sqlite(sql::sqlite::config{.path = "app.db"})
    .build_with(state)
    .start();

coro::task<owl::Response> get_user(const sql::pool<sql::psql>& pg, owl::Path<"id", std::int64_t> id) {
    const auto r = co_await sql::query<"SELECT name FROM users WHERE id = $1">(pg, id.value);
    ...
}
```

v1 limits: no LISTEN/NOTIFY, no statement cache, no checkout timeout, no runtime SQL
strings, no ORM. Never await a statement inside `coro::when_any` (it destroys its losing
children while the statement is parked on a reactor). Inside a transaction body every
statement goes through the `tx`, never the pool — the sqlite pool has one connection and
the transaction holds it, so a pool-sourced statement would wait forever.
