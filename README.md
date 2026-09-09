<div align="center">

# owl

**Header-only C++23 libraries**

A libh2o web framework, a coroutine toolkit, and strings that can be template
arguments. Nothing here is production-ready.

[![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/23)
[![CMake](https://img.shields.io/badge/CMake-4.3-064F8C?logo=cmake&logoColor=white)](#install)
[![header-only](https://img.shields.io/badge/header--only-yes-success)](#)

[Libraries](#libraries) · [Quick start](#quick-start) · [Install](#install)

</div>

> [!WARNING]
> Experimental. APIs move. Do not ship this.

```cpp
#include <owl/owl.h>

struct App final {};

owl::Response ping(owl::RequestView) {
    return owl::Response::ok("pong");
}

int main() {
    auto router = owl::Router<App>::make()
                  .route<"/ping">(owl::get(ping));

    owl::Server<App>::builder()
        .router(std::move(router))
        .config({.port = 8080})
        .build_with(std::make_shared<App>())
        .start();
}
```

## Libraries

| | Target | Include | One-liner |
|---|---|---|---|
| [**owl**](owl/README.md) | `owl::owl` | `<owl/owl.h>` | Typed routes on libh2o; coroutine handlers stay on the event loop |
| [**coro**](coro/README.md) | `owl::coro` | `<coro/coro.h>` | Lazy tasks, generators, schedulers, an I/O reactor |
| [**fstr**](fstr/README.md) | `owl::fstr` | `<fstr/fstr.h>` | String literals as structural NTTPs |
| [**prometheus**](prometheus/README.md) | `owl::prometheus` | `<prometheus/prometheus.h>` | Counters, gauges, histograms, HTTP RED; standalone; `-DOWL_ENABLE_PROMETHEUS=ON` |
| [**sql**](sql/README.md) | `owl::sql` | `<sql/sql.h>` | Async SQLite (on by default) and Postgres (`-DOWL_ENABLE_POSTGRESQL=ON`) for handlers |
| [**redis**](redis/README.md) | `owl::redis` | `<redis/redis.h>` | Async Redis for handlers: pipelined commands, pub/sub streams; `-DOWL_ENABLE_REDIS=ON` |

`owl::owl` pulls `owl::coro`, `owl::fstr`, `libh2o-evloop`, nlohmann_json, OpenSSL, and zlib. `owl::coro` pulls Threads. `owl::fstr` stands alone. `owl::prometheus` stands alone; with `-DOWL_ENABLE_PROMETHEUS=ON` it is `owl::owl` that pulls it, and dispatch records HTTP RED automatically. `owl::sql` pulls `owl::coro`, `owl::fstr`, and sqlite3 and/or libpq per option; with either driver on (sqlite is, unless `-DOWL_ENABLE_SQLITE=OFF`), `owl::owl` pulls it and the server wires per-worker pools. `owl::redis` pulls `owl::coro` and hiredis; with `-DOWL_ENABLE_REDIS=ON`, `owl::owl` pulls it and the server wires a per-worker client.

Each library lives in `include/<name>/` so the prefix is part of the include. Public headers are `#pragma once`.

## Quick start

A handler is a free function. Parameters are extractors; the route pattern is checked against them at compile time.

```cpp
struct App { int hits = 0; };

owl::Response hello(owl::PathView<"name"> name) {
    return owl::Response::ok(std::format("hello {}", name.value));
}

coro::task<owl::Response> hits(owl::State<App> app) {
    co_return owl::Response::json(std::format(R"({{"hits":{}}})", ++app->hits));
}

auto v1 = owl::Router<App>::make()
          .route<"/hits">(owl::get(hits));

auto router = owl::Router<App>::make()
              .nest<"/api/v1">(std::move(v1))
              .route<"/hello/{name}">(owl::get(hello));

owl::Server<App>::builder()
    .router(std::move(router))
    .config({.port = 8080})
    .build_with(std::make_shared<App>());
```

State is bound on `Server<App>` via `build_with`. `Router<App>` and `Server<App>` share the state type, `nest` only takes the same `S`, and a handler naming `State<T>` for another `T` is a compile error, not a runtime 500.

A small REST API -- register, login, posts on sqlite -- lives in [`examples/rest`](examples/rest/README.md), a standalone CMake project that consumes owl the way an application does.

## Install

CMake **4.3**, C++23.

### `add_subdirectory`

```cmake
add_subdirectory(path/to/owl)

target_link_libraries(app PRIVATE owl::owl)     # web: pulls coro, fstr, h2o
# target_link_libraries(app PRIVATE owl::coro)  # tasks only
# target_link_libraries(app PRIVATE owl::fstr)  # NTTPs only
# target_link_libraries(app PRIVATE owl::prometheus)  # needs -DOWL_ENABLE_PROMETHEUS=ON
# target_link_libraries(app PRIVATE owl::sql)  # sqlite by default; -DOWL_ENABLE_POSTGRESQL=ON adds postgres
# target_link_libraries(app PRIVATE owl::redis)  # needs -DOWL_ENABLE_REDIS=ON
```

### `make install`

```sh
cmake -S . -B build
cmake --build build
cmake --install build          # Makefile generators: make -C build install
# cmake --install build --prefix ~/.local
```

Headers land in `<prefix>/include/{owl,coro,fstr}/` (and `prometheus/`, `sql/`, `redis/` if enabled). Then:

```cmake
find_package(owl REQUIRED)
target_link_libraries(app PRIVATE owl::owl)
```

A custom prefix needs `CMAKE_PREFIX_PATH`.

```cpp
#include <owl/owl.h>     // or <coro/coro.h> / <fstr/fstr.h> / <prometheus/prometheus.h>
```

`owl::owl` needs these on the machine (found via CMake / pkg-config):

| | |
|---|---|
| `libh2o-evloop` | pkg-config `libh2o-evloop`, built with `H2O_USE_LIBUV=0` |
| OpenSSL | `find_package(OpenSSL)` — on macOS, `brew install openssl@3` |
| zlib | `find_package(ZLIB)` |
| nlohmann_json | FetchContent when using `add_subdirectory`; `find_package` after install |
| libpq | `find_package(PostgreSQL)` for `-DOWL_ENABLE_POSTGRESQL=ON`; on macOS, `brew install libpq` |
| sqlite3 | `find_package(SQLite3)`, on by default (`-DOWL_ENABLE_SQLITE=OFF` drops it); on macOS, `brew install sqlite` |

`owl::coro` needs Threads. `owl::fstr` needs nothing.
