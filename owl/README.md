<div align="center">

# owl

**Header-only C++23 web framework on libh2o**

Typed handler parameters, a compile-time-validated route trie, and
coroutine handlers that never leave the event loop.

[![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/23)
[![CMake](https://img.shields.io/badge/target-owl::owl-064F8C?logo=cmake&logoColor=white)](#)
[![header-only](https://img.shields.io/badge/header--only-yes-success)](#)

[Handlers](#handlers) · [Responses](#responses) · [Extractors](#extractors) · [State](#state) · [Middleware](#middleware) · [WebSocket](#websocket) · [Server](#server) · [Layout](#layout) · [Roadmap](#roadmap)

</div>

> [!WARNING]
> Part of [owl](../README.md). Experimental. Not production-ready.

```cpp
#include <owl/owl.h>

owl::Response ping(owl::RequestView) {
    return owl::Response::ok("pong");
}

owl::Response hello(owl::RequestView, owl::PathView<"name"> name) {
    return owl::Response::ok(std::format("hello {}", name.value));
}

struct App final {};

auto router = owl::Router<App>::make()
              .route<"/ping">(owl::get(ping))
              .route<"/hello/{name}">(owl::get(hello));
```

Link `owl::owl`. Pulls `libh2o-evloop`, libwslay, `owl::coro`, `owl::fstr`, nlohmann_json, OpenSSL, and zlib.

## Handlers

A handler is any free function returning `owl::Response`, or `coro::task` of one. Its parameters are extracted from the request; the route pattern is checked against them at compile time, so asking for a path parameter the pattern does not declare is a build error, not a 404.

```cpp
coro::task<owl::Response> calc(const owl::loop_scheduler& loop, owl::Path<"n", int> n) {
    co_await loop.schedule();
    co_return owl::Response::ok(std::format("{}", n.value * 2));
}
```

## Responses

`Response` is untyped so middleware can `co_await next()` and return the same object. Factories fix status and body together: `ok`, `json`, `body`, `stream`, `sse`, `error`, `no_content`, `redirect`. Headers chain on afterwards.

```cpp
owl::Response login(owl::RequestView) {
    return owl::Response::ok("ok")
           .header("x-trace", trace_id)
           .set_cookie({.name = "sid", .value = token, .http_only = true});
}
```

`set_cookie` appends instead of replacing, because `Set-Cookie` is the one response header that legitimately repeats. `sse()` frames each chunk as `data: …\n\n` and sets `cache-control: no-cache`. It does not send `Connection` — that header is hop-by-hop and belongs to h2o.

## Extractors

| Parameter                            | Source                  | Missing / malformed |
|--------------------------------------|-------------------------|---------------------|
| `RequestView`                        | the request itself      | —                   |
| `PathView<"n">` / `Path<"n", T>`     | path segment            | 404                 |
| `QueryView<"q">` / `Query<"q", T>`   | query string            | 400                 |
| `HeaderView<"h">` / `Header<"h", T>` | header                  | 400                 |
| `BodyView`                           | raw body                | —                   |
| `Json<T>`                            | JSON body               | 415 / 400 / 422     |
| `State<T>`                           | router state            | 500                 |
| `loop_scheduler`                     | the worker's event loop, bound with no copy when taken as `const&` | 500                 |
| `const sql::pool<sql::psql>&`        | the worker's postgres pool (`-DOWL_ENABLE_POSTGRESQL=ON`) | 500                 |
| `const sql::pool<sql::sqlite>&`      | the worker's sqlite pool (on by default)                  | 500                 |
| `const redis::client&`               | the worker's Redis client (`-DOWL_ENABLE_REDIS=ON`)         | 500                 |

A custom extractor is one `FromContext` specialization — the built-ins in `extract/from_context.h` are the same protocol, and make good reference. The parameter type is the value the handler receives; the specialization answers either that value or a `KickToken`, which carries any status, so `401` needs no special support:

```cpp
struct Bearer {
    std::string_view token;
};

template <>
struct owl::FromContext<Bearer> {
        template <typename S>
        std::expected<Bearer, owl::KickToken> operator()(const owl::Context<S>&, const owl::Request& req) const {
        const auto auth = req.header("authorization");  // header names match case-insensitively
        if (!auth || !auth->starts_with("Bearer ")) {
            return std::unexpected(owl::KickToken{"missing bearer token", 401});
        }
        return Bearer{auth->substr(7)};
    }
};

owl::Response whoami(Bearer token) {
    return owl::Response::ok(std::string{token.token});
}

struct App final {};

auto router = owl::Router<App>::make()
              .route<"/whoami">(owl::get(whoami));
```

The `std::unexpected` wrap is mandatory — `std::expected` converts from `std::unexpected<KickToken>`, never from a bare error. The token is a view into the request's header table, which outlives the handler, so no copy is needed. Declare the specialization in a header of yours next to the type and include it before the route that uses it: the handler's parameters are checked where the route is declared, so a specialization that is not visible yet is a build error, not a runtime surprise.

## State

State is bound once, at the end:

```cpp
struct AppState { int hits = 0; };

owl::Response hits(owl::State<AppState> app) {
    return owl::Response::json(std::format(R"({{"hits":{}}})", ++app->hits));
}

auto v1 = owl::Router<AppState>::make()
          .route<"/hits">(owl::get(hits));

auto router = owl::Router<AppState>::make()
              .nest<"/api/v1">(std::move(v1))
              .route<"/hits">(owl::get(hits));

owl::Server<AppState>::builder()
    .router(std::move(router))
    .config({.port = 8080})
    .build_with(std::make_shared<AppState>());
```

State is bound on `Server<AppState>` via `build_with`, so a server that never got its state fails to compile rather than at startup. `Router<S>` and `Server<S>` share `S`; a handler naming `State<T>` for any other `T` is a compile error. `nest` only accepts the same `S`.

## Middleware

A middleware coroutine takes `const Request&` and `Next<S>`, optionally `const Context<S>&` when it needs state, and returns `task<Response>`. Call `next` to continue; return a response to kick. Headers apply on the way out.

```cpp
coro::task<owl::Response> timing(const owl::Request& req, owl::Next<AppState> next) {
    const auto start = std::chrono::steady_clock::now();
    owl::Response res = co_await next(req);
    res.header("x-elapsed-ms", std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count()));
    co_return std::move(res);
}

auto router = owl::Router<AppState>::make()
              .layer(timing)
              .route<"/ping">(owl::get(ping));
```

`Server::Builder::layer` is the outermost chain (MatchedChains slot 0). Nested `Router::layer` runs only under that prefix.

## WebSocket

A path takes a send/recv coroutine or a shared controller. Extractors run before the upgrade — the request is gone at 101 — so they must own their values (`Path<"n", std::string>`, not `PathView`). `const T&` is allowed only for driver and loop refs, which outlive the connection on their worker. `owl::get` and `.ws` may share a pattern; a GET without Upgrade on a WS-only path is 404. A handshake that offers a version other than 13 gets 426 with `Sec-WebSocket-Version: 13`; a missing or malformed key gets 400. One controller instance is built at registration and reached from every worker, so it must be safe for concurrent use, the same contract `State<T>` carries. `send` is awaitable so it can gain backpressure later; it does not have it yet.

A working echo, rooms, and chat page is [`examples/ws`](../examples/ws/README.md).

```cpp
coro::task<void> room(owl::ws::Socket sock, owl::Path<"id", int> id) {
    while (const auto msg = co_await sock.recv()) {
        co_await sock.send(std::format("{}: {}", id.value, msg->data()));
    }
}

struct Chat final {
    using Extractors = std::tuple<owl::Path<"room", std::string>>;
    coro::task<void> on_message(owl::ws::Socket, owl::ws::Message, owl::Path<"room", std::string>);
};

owl::Router<App>::make()
    .route<"/chat">(owl::get(page))
    .ws<"/chat/{room}", Chat>()
    .ws<"/rooms/{id}">(room);
```

### Controller extractors

A coroutine lists extractors on the function. A controller cannot: `on_message` may be overloaded, templated, or defaulted, and deduction from that would fail unreadably. So the pack is a type alias, not inferred:

```cpp
using Extractors = std::tuple<owl::Path<"room", std::string>, owl::Header<"authorization", std::string>>;
```

Absent means none. Each method may take the pack or skip it (`on_disconnect` often skips). The values live in the per-connection frame, never on the shared instance — a member would race the next handshake.

`on_disconnect` runs for every connection whose `on_connect` started, even when a method threw; the exception is then rethrown, and the connection closes with 1011. A coroutine handler that throws closes with 1011 too.

## Prometheus

See [`prometheus/README.md`](../prometheus/README.md). Build with `-DOWL_ENABLE_PROMETHEUS=ON`: `owl::owl` then depends on `owl::prometheus` (which itself pulls nothing), and the server records HTTP RED itself — every exchange through dispatch under its route pattern, thrown handlers as `500`, and unmatched requests (`404`/`405`, dispatch failures) counted without a duration. Your handler only serves `owl::prometheus::dump()` as `text/plain; version=0.0.4; charset=utf-8`.

## SQL

See [`sql/README.md`](../sql/README.md). With a driver enabled (SQLite is, by default; Postgres with `-DOWL_ENABLE_POSTGRESQL=ON`), `owl::owl` pulls `owl::sql`, `Config` grows `psql` / `sqlite`, and every worker gets its own pools on its own loop. A handler takes the pool by `const&` and awaits `sql::query<"...">(pool, args...)`; the wait never leaves the worker.

See [`redis/README.md`](../redis/README.md). With `-DOWL_ENABLE_REDIS=ON`, `owl::owl` pulls `owl::redis`, `Config` grows `redis`, and every worker gets its own client on its own loop. A handler takes it by `const&` and awaits `rd.command("GET", key)` or one of the typed helpers on it; a subscription is `redis::subscribe(rd, {channel})`, ended by scope or a `std::stop_token`.

## Server

```cpp
owl::Server<AppState> server = owl::Server<AppState>::builder()
                     .router(std::move(router))
                     .config({.port = 8080, .threads = 4})
                     .build_with(std::make_shared<AppState>());
server.start();
```

`router` is a `Router<S>`. `threads` starts N workers — Node cluster, in-process. Worker 0 runs on the calling thread; the rest are extra threads. Each worker is a single-threaded event loop with its own `SO_REUSEPORT` listener. They share the router. `.thread(N)` is the sanctioned post-config tweak; a second `.config()` throws because it would reset every field not repeated.

`Config::make(argc, argv)` parses CLI over `OWL_*` env over defaults. Bad integers, unknown flags, leftover positionals, and empty values throw `std::invalid_argument`.

| | env | flag | default |
|---|---|---|---|
| bind | `OWL_ADDRESS` | `-a` / `--address` | `127.0.0.1` |
| port | `OWL_PORT` | `-p` / `--port` | `8080` |
| workers | `OWL_THREADS` | `-t` / `--threads` | `1` |
| listen backlog | `OWL_BACKLOG` | `-b` / `--backlog` | `1024` |
| postgres DSN | `OWL_PG` | `--pg` | unset |
| sqlite path | `OWL_SQLITE` | `--sqlite` | unset |
| redis `host` or `host:port` | `OWL_REDIS` | `--redis` | unset |

Driver fields are `std::optional` and exist only under their `OWL_ENABLE_*` macros. Unset means the extractor kicks 500, not a connection to localhost.

Do not block the loop. Offload with a pool; `co_await loop.schedule()` or `loop.post(handle)` hops back (Node's `setImmediate` from another thread). Resume is always on the **same** worker.

Each box below is one turn of that worker's `h2o_evloop_run`:

<p align="center">
  <img src="docs/workers.svg" alt="Workers as a Node-style cluster, each with an event loop. The loop polls, dispatches, hops off-thread work back, then runs handlers and sends." width="720"/>
</p>

## Layout

`include/owl/` is layered bottom-up — nothing lower includes anything higher:

| Directory  | Holds                                                                                       |
|------------|---------------------------------------------------------------------------------------------|
| `core/`    | vocabulary types: `Method`, `Config`, `State`, `Context`, `KickToken` |
| `util/`    | `pool_map`, string helpers                                                                  |
| `http/`    | `Request`, `Response`, cookies, reason phrases; `detail/` for send/finish                   |
| `extract/` | parsing, extractor types, and the `FromContext` dispatch                                    |
| `routing/` | the route trie, `Router`, middleware                                                        |
| `coro/`    | `loop_scheduler`, which hops work back onto the request's event loop                        |
| `ws/`      | `Message`, `Socket`, controller sugar; handshake and engine in `detail/`                    |
| `server.h` | h2o workers, listeners, and the dispatch entry point                                        |

`owl.h` includes all of it.

## Roadmap

Not started. Each item should sit on `coro` + the event loop the way handlers already do — no extra thread pools, no blocking the worker.

|                       | Sketch                                                                                |
|-----------------------|---------------------------------------------------------------------------------------|
| **WebSocket**         | In. Remaining: backpressure on `send`; controller extractors stay a declared `using Extractors` tuple (not deduced from `on_message`). |
| **Redis**             | RESP client on `coro::native_reactor`. Extractor or `State<>` for a shared pool.      |
| **Static files**      | Route that sendfiles a directory. Range requests later.                               |
| **TLS**               | HTTPS as a `Server::Builder` switch; h2o already links OpenSSL.                       |
| **Graceful shutdown** | Stop listeners, drain in-flight handlers, then join workers.                          |
