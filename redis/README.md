<div align="center">

# redis

**Header-only C++23 async Redis for owl handlers**

One pipelined connection per worker, replies as RESP3 values, pub/sub as an
async generator, never a blocked event loop.

[![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/23)
[![CMake](https://img.shields.io/badge/target-owl::redis-064F8C?logo=cmake&logoColor=white)](#)
[![header-only](https://img.shields.io/badge/header--only-yes-success)](#)

[Commands](#commands) · [Replies](#replies) · [Errors](#errors) · [Pub/Sub](#pubsub) · [Client](#client) · [In owl](#in-owl) · [Tests](#tests)

</div>

> [!WARNING]
> Part of [owl](../README.md). Experimental. Not production-ready.

```cpp
coro::task<owl::Response> cached(const redis::client& rd, owl::Path<"id", std::int64_t> id) {
    const auto key = "user:" + std::to_string(id.value);
    if (const auto hit = co_await rd.get(key)) co_return owl::Response::json(*hit);
    const std::string body = co_await render_user(id.value);
    co_await rd.set(key, body, std::chrono::minutes{5});
    co_return owl::Response::json(body);
}
```

Link `owl::redis` (pulls `owl::coro` and hiredis, found with pkg-config). It is
off by default: `-DOWL_ENABLE_REDIS=ON`. `redis/redis.h` is the umbrella.
Every connection speaks RESP3, so the server must be Redis 6 or newer.

## Commands

Two entry points, one shape: the command is its argv. They are members,
because a client is the one thing a command needs -- there is a single
client type, unlike sql, where `query<"...">` runs against any driver's pool
or transaction and so has to be told which.

```cpp
auto r = co_await rd.try_command("SET", key, value, "EX", 60);   // std::expected<redis::reply, redis::error>
auto v = co_await rd.command("GET", key);                        // redis::reply, throws redis::error
```

Arguments may be `std::string`, `std::string_view`, C strings and literals,
integers (not `bool` or `char`), floating point, `std::span<const std::byte>`,
`std::vector<std::byte>`, or any input range of those, which expands in place:
`command(rd, "DEL", keys)` with a `std::vector<std::string>` sends one `DEL`
with every key. Anything else fails to compile.

Arguments are borrowed for the life of the task: await in the same expression
that built it. `co_await redis::command(rd, "GET", key)` is right; naming the
task and awaiting it later with temporaries as arguments dangles.

Seven helpers wrap the calls a cache, a rate limiter and pub/sub make all the
time, each as a `try_` (returns `std::expected`) and a throwing twin:

| | Sends | Returns |
|---|---|---|
| `rd.get(key)` | `GET` | `std::optional<std::string>` |
| `rd.set(key, value)` / `rd.set(key, value, 1500ms)` | `SET` / `SET ... PX` | `void` |
| `rd.del(key)` / `rd.del(keys)` | `DEL` | `std::int64_t` removed |
| `rd.incr(key)` | `INCR` | `std::int64_t` |
| `rd.expire(key, 60s)` | `EXPIRE` | `bool`, the key existed |
| `rd.eval(script, keys, args)` | `EVAL` with `numkeys` from `keys` | `redis::reply` |
| `rd.publish(channel, payload)` | `PUBLISH` | `std::int64_t` receivers |

`keys` and `args` are ranges. Everything else is `rd.command(...)`.

## Replies

`redis::reply` owns one RESP3 value; `type()`, `is_null()`, `size()`,
`operator[]` (a borrowed `reply_view` with the same accessors), `raw()` for
the `redisReply*`, and `as<T>()`:

| `T` | Reads |
|---|---|
| `std::string` | status, string, verbatim, bignum; integer and real as text |
| `std::string_view` | the text kinds, as a view into the reply |
| integers other than `bool` / `char` | integer; text parsed as a whole number; range-checked |
| `double`, `float` | real; integer; text parsed |
| `bool` | boolean; integer 0 / 1 |
| `std::optional<U>` | nil is `std::nullopt` |
| `std::vector<U>` | array, set, push; a map flattens to alternating keys and values, as RESP2 does |

A pairing that cannot be honest -- nil into a non-optional, an error element
into anything, an array into an int -- throws `redis::error` with
`kind() == conversion`, because reading a reply is synchronous.

## Errors

`redis::error` derives `std::runtime_error` and carries `kind()`; for a server
error reply, `prefix()` is its first word (`ERR`, `WRONGTYPE`, `NOSCRIPT`),
which is enough to fall back from `EVALSHA` to `EVAL`. The `try_` functions
never throw for a server or link failure.

| `kind()` | Meaning | Connection afterwards |
|---|---|---|
| `connect` | opening failed: TCP, `HELLO`, `AUTH` or `SELECT` refused | none was made; the next command retries |
| `connection` | the link died mid-use, or was dropped because another command timed out | discarded; the next command reconnects |
| `command` | the server answered an error reply | kept |
| `timeout` | `connect_timeout` or `command_timeout` passed | finished and discarded |
| `closed` | a command on a closed client | -- |
| `conversion` | a reply could not be produced as asked (thrown) | kept |

## Pub/Sub

```cpp
auto messages = redis::subscribe(rd, {"news", "alerts"});
while (const auto m = co_await messages.next()) {
    use(m->channel, m->payload);
}
```

`subscribe` is a free function rather than a member because it is not a
command: it does not use the client's connection, it borrows only its config
and reactor to open one of its own. Each subscription is therefore its own
connection, authenticated and on the same database as the client. The stream yields every `message` push and skips the
confirmations. A broken subscriber connection throws `redis::error` at
`next()`.

Ending a subscription depends on whether a pull is pending:

- **No `next()` pending:** destroy the generator. Its frame dies at the yield
  it is parked on, the connection with it, and the server drops the
  subscription on EOF. There is no unsubscribe call.
- **A `next()` pending:** pass a `std::stop_token` and request a stop. The
  generator cancels the parked read through the reactor, and that `next()`
  completes with `std::nullopt` without anything being closed to achieve
  it. Never destroy a generator whose `next()` is pending: its frame is
  parked in the reactor. Where the stop may be requested from is the
  reactor's rule: `coro::native_reactor` accepts a cancel from any thread,
  owl's `loop_reactor` only from its own loop.

```cpp
std::stop_source stop;
auto messages = redis::subscribe(rd, {"news"}, stop.get_token());
// elsewhere, when done: stop.request_stop();
```

A consumer that stops pulling but keeps the generator alive leaves the socket
to fill until the server's `client-output-buffer-limit` closes it.

## Client

```cpp
coro::native_reactor reactor;            // or owl's loop_reactor
redis::client rd{{.host = "127.0.0.1", .port = 6379, .password = "s3cret", .db = 1}, coro::reactor_ref{reactor}};
```

A client is used from one thread and has no locks. It holds one command
connection, opened by the first command; every command appends to it and
replies are matched in order, so concurrent handlers are pipelined without
asking. A push that arrives on it is dropped. A failure drops the connection
and fails every command in flight (the one that timed out with `timeout`,
the others with `connection`); the next command reconnects, with no backoff.
`close()` refuses new commands and lets in-flight ones finish. The reactor
must outlive the client, and a client must outlive its commands.

Config: `host`, `port`, `username` (empty is the default user), `password`
(empty is no `AUTH`), `db`, `connect_timeout` (5 s, covers the handshake),
`command_timeout` (none).

## In owl

```cpp
owl::Server<App>::builder()
    .router(std::move(router))
    .config({.port = 8080})
    .with_redis({.host = "127.0.0.1", .port = 6379})
    .build_with(state)
    .start();
```

Every worker gets its own client on its own loop reactor; handlers extract
`const redis::client&`. A client the builder never wired kicks 500.

## Not in this version

TLS, unix sockets, cluster and sentinel, `MULTI`/`EXEC` batching,
`PSUBSCRIBE`, blocking commands (`BLPOP`, `XREAD BLOCK`), client-side
caching, reconnect backoff, RESP2 fallback, async name resolution (hiredis
resolves synchronously, as libpq does), a typed layer beyond the seven
helpers, wiring an owl request's abort to a subscription's stop token.

Note for anyone linking `owl::owl`: `libh2o-evloop.a` bundles its own
hiredis 1.2.0 for h2o's `h2o_redis_client_t`. Those objects are linked only
if something references `h2o_redis_*`; owl never does, so brew's hiredis is
the only one in the process. Keep it that way.

## Tests

`redis/tests/` mirrors the headers. The client, connection, command and
pub/sub suites run against a scripted fake server on a loopback port and
need nothing installed. The live suite skips unless `OWL_TEST_REDIS` names a
server:

```sh
brew services start redis
OWL_TEST_REDIS=127.0.0.1:6379 ctest --test-dir cmake-build-redis -R redis_ --output-on-failure
```
