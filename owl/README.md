<div align="center">

# owl

**Header-only C++23 web framework on libh2o**

Typed handler parameters, a compile-time-validated route trie, and
coroutine handlers that never leave the event loop.

[![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/23)
[![CMake](https://img.shields.io/badge/target-owl::owl-064F8C?logo=cmake&logoColor=white)](#)
[![header-only](https://img.shields.io/badge/header--only-yes-success)](#)

[Handlers](#handlers) · [Responses](#responses) · [Extractors](#extractors) · [State](#state) · [Middleware](#middleware) · [Server](#server) · [Layout](#layout) · [Roadmap](#roadmap)

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

auto router = owl::Router<>::make()
              .route<"/ping">(owl::get(ping))
              .route<"/hello/{name}">(owl::get(hello));
```

Link `owl::owl`. Pulls `libh2o-evloop`, `owl::coro`, `owl::fstr`, nlohmann_json, OpenSSL, and zlib.

## Handlers

A handler is any free function returning `owl::Response`, or `coro::task` of one. Its parameters are extracted from the request; the route pattern is checked against them at compile time, so asking for a path parameter the pattern does not declare is a build error, not a 404.

```cpp
coro::task<owl::Response> calc(owl::loop_scheduler loop, owl::Path<"n", int> n) {
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
| `loop_scheduler`                     | the worker's event loop | 500                 |

A custom extractor is one `FromContext` specialization — the built-ins in `extract/from_context.h` are the same protocol, and make good reference. The parameter type is the value the handler receives; the specialization answers either that value or a `KickToken`, which carries any status, so `401` needs no special support:

```cpp
struct Bearer {
    std::string_view token;
};

template <>
struct owl::FromContext<Bearer> {
    std::expected<Bearer, owl::KickToken> operator()(const owl::Context&, const owl::Request& req) const {
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

auto router = owl::Router<>::make()
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
              .route<"/hits">(owl::get(hits))
              .with_state(std::make_shared<AppState>());
```

`with_state` consumes the `Router<AppState>` and hands back a `Router<>`, which is the only thing `Server::Builder::router` accepts — so a router that never got its state fails to compile rather than at startup. A handler naming `State<T>` for any other `T` is rejected where it is routed.

`nest` takes a router over the same state type (`std::same_as`); bind the state once, on the outermost router.

## Middleware

A middleware coroutine takes `const Request&` and `Next`, optionally `State<S>`, and returns `task<Response>`. Call `next` to continue; return a response to kick. Headers apply on the way out.

```cpp
coro::task<owl::Response> timing(const owl::Request& req, owl::Next next) {
    const auto start = std::chrono::steady_clock::now();
    owl::Response res = co_await next(req);
    res.header("x-elapsed-ms", std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count()));
    co_return std::move(res);
}

auto router = owl::Router<>::make()
              .layer(timing)
              .route<"/ping">(owl::get(ping));
```

`Server::Builder::layer` is the outermost chain (MatchedChains slot 0). Nested `Router::layer` runs only under that prefix.

## Server

```cpp
owl::Server server = owl::Server::builder()
                     .router(std::move(router))
                     .config({.port = 8080})
                     .thread(4)
                     .build();
server.start();
```

`router` must be a `Router<>`. `.thread(N)` starts N workers — Node cluster, in-process. Worker 0 runs on the calling thread; the rest are extra threads. Each worker is a single-threaded event loop with its own `SO_REUSEPORT` listener. They share the router.

Do not block the loop. Offload with a pool; `co_await loop.schedule()` or `loop.post(handle)` hops back (Node's `setImmediate` from another thread). Resume is always on the **same** worker.

Each box below is one turn of that worker's `h2o_evloop_run`:

<p align="center">
  <img src="docs/workers.svg" alt="Workers as a Node-style cluster, each with an event loop. The loop polls, dispatches, hops off-thread work back, then runs handlers and sends." width="720"/>
</p>

## Layout

`include/owl/` is layered bottom-up — nothing lower includes anything higher:

| Directory  | Holds                                                                                       |
|------------|---------------------------------------------------------------------------------------------|
| `core/`    | vocabulary types with no local dependencies: `Method`, `State`, `KickToken`, pool allocator |
| `util/`    | `pool_map`, string helpers                                                                  |
| `http/`    | `Request`, `Response`, cookies, reason phrases; `detail/` for send/finish                   |
| `extract/` | parsing, extractor types, and the `FromContext` dispatch                                    |
| `routing/` | the route trie, `Router`, middleware                                                        |
| `coro/`    | `loop_scheduler`, which hops work back onto the request's event loop                        |
| `server.h` | h2o workers, listeners, and the dispatch entry point                                        |

`owl.h` includes all of it.

## Roadmap

Not started. Each item should sit on `coro` + the event loop the way handlers already do — no extra thread pools, no blocking the worker.

|                       | Sketch                                                                                |
|-----------------------|---------------------------------------------------------------------------------------|
| **WebSocket**         | ---                                                                                   |
| **Redis**             | RESP client on `coro::native_reactor`. Extractor or `State<>` for a shared pool.      |
| **Postgres**          | Async query client, same I/O model as Redis.                                          |
| **Static files**      | Route that sendfiles a directory. Range requests later.                               |
| **TLS**               | HTTPS as a `Server::Builder` switch; h2o already links OpenSSL.                       |
| **Graceful shutdown** | Stop listeners, drain in-flight handlers, then join workers.                          |
