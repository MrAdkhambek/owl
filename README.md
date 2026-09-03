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

owl::Response ping(owl::RequestView) {
    return owl::Response::ok("pong");
}

int main() {
    auto router = owl::Router<>::make()
                  .route<"/ping">(owl::get(ping));

    owl::Server::builder()
        .router(std::move(router))
        .config({.port = 8080})
        .build()
        .start();
}
```

## Libraries

| | Target | Include | One-liner |
|---|---|---|---|
| [**owl**](owl/README.md) | `owl::owl` | `<owl/owl.h>` | Typed routes on libh2o; coroutine handlers stay on the event loop |
| [**coro**](coro/README.md) | `owl::coro` | `<coro/coro.h>` | Lazy tasks, generators, schedulers, an I/O reactor |
| [**fstr**](fstr/README.md) | `owl::fstr` | `<fstr/fstr.h>` | String literals as structural NTTPs |

`owl::owl` pulls `owl::coro`, `owl::fstr`, `libh2o-evloop`, nlohmann_json, OpenSSL, and zlib. `owl::coro` pulls Threads. `owl::fstr` stands alone.

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
              .route<"/hello/{name}">(owl::get(hello))
              .with_state(std::make_shared<App>());
```

`with_state` consumes `Router<App>` and returns `Router<>` — the only type `Server` accepts. Nesting requires the same state type; bind state once, on the outermost router.

The demo binary is `owl_demo` from `main.cpp`.

## Install

CMake **4.3**, C++23.

### `add_subdirectory`

```cmake
add_subdirectory(path/to/owl)

target_link_libraries(app PRIVATE owl::owl)     # web: pulls coro, fstr, h2o
# target_link_libraries(app PRIVATE owl::coro)  # tasks only
# target_link_libraries(app PRIVATE owl::fstr)  # NTTPs only
```

### `make install`

```sh
cmake -S . -B build
cmake --build build
cmake --install build          # Makefile generators: make -C build install
# cmake --install build --prefix ~/.local
```

Headers land in `<prefix>/include/{owl,coro,fstr}/`. Then:

```cmake
find_package(owl REQUIRED)
target_link_libraries(app PRIVATE owl::owl)
```

A custom prefix needs `CMAKE_PREFIX_PATH`.

```cpp
#include <owl/owl.h>     // or <coro/coro.h> / <fstr/fstr.h>
```

`owl::owl` needs these on the machine (found via CMake / pkg-config):

| | |
|---|---|
| `libh2o-evloop` | pkg-config `libh2o-evloop`, built with `H2O_USE_LIBUV=0` |
| OpenSSL | `find_package(OpenSSL)` — on macOS, `brew install openssl@3` |
| zlib | `find_package(ZLIB)` |
| nlohmann_json | FetchContent when using `add_subdirectory`; `find_package` after install |

`owl::coro` needs Threads. `owl::fstr` needs nothing.
