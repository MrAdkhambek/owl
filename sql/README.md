<div align="center">

# sql

**Header-only C++23 async SQL for owl handlers**

Postgres over async libpq, SQLite on a pinned thread per connection, one
pool, one `query<"...">`, never a blocked event loop.

[![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/23)
[![CMake](https://img.shields.io/badge/target-owl::sql-064F8C?logo=cmake&logoColor=white)](#)
[![header-only](https://img.shields.io/badge/header--only-yes-success)](#)

[Queries](#queries) · [Results](#results) · [Errors](#errors) · [Transactions](#transactions) · [Pools](#pools) · [In owl](#in-owl) · [Drivers](#drivers)

</div>

> [!WARNING]
> Part of [owl](../README.md). Experimental. Not production-ready.

```cpp
coro::task<owl::Response> get_user(const sql::pool<sql::psql>& pg, owl::Path<"id", std::int64_t> id) {
    const auto r = co_await sql::try_query<"SELECT name FROM users WHERE id = $1">(pg, id.value);
    if (!r) co_return owl::Response::ok(r.error().what(), 500);
    if (r->rows() == 0) co_return owl::Response::error(404);
    co_return owl::Response::ok(std::string{(*r)[0][0].as<std::string_view>()});
}
```

Link `owl::sql` (pulls `owl::coro`, `owl::fstr`, and libpq / sqlite3 per option). Build with
`-DOWL_ENABLE_POSTGRESQL=ON` and/or `-DOWL_ENABLE_SQLITE=ON`. `sql/sql.h` is the umbrella;
each driver header comes in only under its option.

## Queries

Four free functions, one shape. The query text is a template argument, so the
`$N` placeholders are counted at compile time: a count that does not match the
arguments, or a hole such as `$1, $3`, is a build error.

```cpp
auto r  = co_await sql::query<"SELECT * FROM users WHERE id = $1">(pg, id);        // psql::result, throws sql::error
auto tr = co_await sql::try_query<"SELECT ...">(pg, id);                            // std::expected<psql::result, sql::error>
auto n  = co_await sql::execute<"INSERT INTO users(name) VALUES ($1)">(pg, name);   // rows affected, throws
auto tn = co_await sql::try_execute<"...">(pg, name);                               // std::expected<std::uint64_t, sql::error>
```

`$1..$N` works for both drivers (sqlite accepts it too). One statement per
literal. Arguments may be `bool`, integers, floats, `std::string`,
`std::string_view`, C strings and literals, `sql::blob` (`std::vector<std::byte>`),
`sql::blob_view`, `std::nullopt`, and `std::optional` of any of those (empty is NULL).
Anything else fails to compile.

Arguments are borrowed for the life of the task: await in the same expression
that built it. `co_await sql::query<"...">(pg, x)` is right; naming the task and
awaiting it later with temporaries as arguments dangles.

## Results

```cpp
r.rows(); r.columns(); r.affected();
for (const auto row : r) { ... }
r[0][0].as<int>();                         // by index
r[0]["name"].as<std::string_view>();       // by column name
r[0][2].is_null();
r[0][2].as<std::optional<int>>();          // NULL -> nullopt
auto [id, name] = r[0].as<int, std::string>();   // exactly the row's width
```

`psql::result` owns the `PGresult`; cells are views into it and parse on demand
(`raw()` hands the `PGresult*` back for anything `PQ*` can do). `sqlite::result`
is rows copied on the connection's thread in sqlite's own storage classes:
an INTEGER reads as any integer type (range-checked), `bool`, or a float; TEXT
as a string, or parsed into a number; BLOB as `sql::blob`; every other pairing
throws rather than guesses. NULL into a non-optional throws. All of those throw
`sql::error` with `kind() == conversion`.

`affected()` is the count of an INSERT, UPDATE, DELETE or MERGE and 0 for
anything else on both drivers (libpq's own count for a SELECT is the rows
returned; the driver reads it as 0).

## Errors

`sql::error` derives `std::runtime_error` and carries `kind()`, `sqlstate()`
(psql, `"UNKNOWN"` when libpq has none) and `code()` (psql `ExecStatusType`,
sqlite extended result code). Drivers build it from the C API's status; the
`try_` functions never throw internally.

| `kind()` | Meaning | Connection afterwards |
|---|---|---|
| `connect` | opening failed | none was made; the next checkout retries |
| `connection` | the link died mid-use | discarded by the pool |
| `query` | the server rejected the statement | kept |
| `timeout` | `query_timeout` / `connect_timeout` passed | finished and discarded |
| `closed` | checkout on a closed pool | — |
| `conversion` | a cell could not be produced as asked (thrown) | kept |

## Transactions

```cpp
const auto total = co_await sql::transaction(pg, [](auto tx) -> coro::task<std::expected<int, sql::error>> {
    const auto r = co_await sql::try_query<"SELECT sum(amount) FROM orders">(tx);
    if (!r) co_return std::unexpected(r.error());
    co_return (*r)[0][0].template as<int>();
});
```

`BEGIN`, the body, then `COMMIT` on a value or `ROLLBACK` on an error, returned
as-is. A body that throws is rolled back and rethrown with the connection kept.
The body must return `coro::task<std::expected<T, sql::error>>`, where `T` may be
`void`, and takes the transaction source by value or by reference; it is a `source`, so every query
function works on it. `transaction` takes only a pool: nesting is a compile error.

## Pools

```cpp
coro::native_reactor reactor;            // or owl's loop_reactor
sql::pool<sql::psql> pg{{.dsn = "postgres://localhost/app", .connections = 4}, sql::reactor_ref{reactor}};

coro::static_thread_pool hop{1};         // or owl's loop_scheduler
sql::pool<sql::sqlite> db{{.path = "app.db"}, sql::scheduler_ref{hop}};
```

A pool is used from one thread and has no locks. Connections open lazily up to
`connections`; a checkout takes an idle one or waits FIFO; a released connection
goes straight to the first waiter; a broken one is discarded and replaced.
`close()` fails waiting checkouts with `closed`. The reactor or scheduler must
outlive the pool, and a pool must outlive its leases.

## In owl

```cpp
owl::Server<App>::builder()
    .router(std::move(router))
    .config({.port = 8080})
    .with_psql({.dsn = "postgres://localhost/app", .connections = 4})
    .with_sqlite({.path = "app.db"})
    .build_with(state)
    .start();
```

Every worker gets its own pools, built on that worker's `loop_reactor` and
`loop_scheduler`, so a server with four threads and `connections = 4` opens up
to sixteen postgres connections. Handlers extract `const sql::pool<sql::psql>&`
or `const sql::pool<sql::sqlite>&`; a pool the builder never wired kicks 500.

## Drivers

| | `sql::psql` | `sql::sqlite` |
|---|---|---|
| I/O | async libpq, waits through `sql::reactor_ref` | one thread per connection, hops back through `sql::scheduler_ref` |
| Config | `dsn`, `connections`, `connect_timeout` (5 s), `query_timeout` (none) | `path`, `connections`, `busy_timeout` (5 s) |
| Results | views into `PGresult`, text format | copied rows in sqlite storage classes |
| Statements | `PQsendQueryParams` per call | prepared once per literal per connection |

WAL is not set by the library: run `execute<"PRAGMA journal_mode=WAL">` once if
you want it. Not in this version: prepared-statement cache for psql, binary
format, pipeline mode, cancellation, streaming rows, named parameters, dollar
quoting inside a literal, savepoints, timestamps / arrays / json as typed cells,
reconnect backoff.

## Tests

`sql/tests/` mirrors the headers. The psql suite skips unless
`OWL_TEST_PSQL_DSN` is set:

```sh
brew services start postgresql@17
/opt/homebrew/opt/postgresql@17/bin/createdb owl_test
OWL_TEST_PSQL_DSN=postgres://localhost/owl_test ctest --test-dir cmake-build-debug -R sql_ --output-on-failure
```
