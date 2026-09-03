<div align="center">

# fstr

**String literals as structural NTTPs**

`std::string_view` cannot be a template argument. This can.

[![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/23)
[![CMake](https://img.shields.io/badge/target-owl::fstr-064F8C?logo=cmake&logoColor=white)](#)
[![header-only](https://img.shields.io/badge/header--only-yes-success)](#)

[API](#api) · [Notes](#notes)

</div>

> [!WARNING]
> Part of [owl](../README.md). Experimental. Not production-ready.

```cpp
#include <fstr/fstr.h>

template <fstr::fstr S>
constexpr std::size_t nttp_size = S.size();

static_assert(nttp_size<"hello"> == 5);
```

Link `owl::fstr`. No other dependencies.

## API

| | |
|---|---|
| `fstr::fstr<N>` | Public `char data[N]` (NUL included in `N`). Structural. |
| Constructor | `consteval` from `const char (&)[N]`. Runtime strings cannot become NTTPs. |
| Embedded NUL | Rejected (`throw` in `consteval`) so consumers are not truncated. |
| `view()` | `string_view` of the `N-1` payload bytes |
| `size()` / `empty()` / `begin()` / `end()` | |
| `operator std::string_view` | Implicit |
| `operator std::string` | Explicit |
| `std::format` | Formats as the payload string, not as a range of chars |
| `std::hash` | Hashes `view()`, so it works as an unordered container key |
| `operator<=>` | Same-length only — different `N` are different types |
| CTAD | `fstr(const char (&)[N]) -> fstr<N>` |

```cpp
constexpr fstr::fstr hello{"hello"};

static_assert(hello.size() == 5);
static_assert(hello.view() == "hello");
static_assert(nttp_size<"ping"> == 4);
```

owl uses this for route patterns and extractor names: `PathView<"id">`, `route<"/hello/{name}">`.

## Notes

Prefer `view()` over writing `data`. `data` is public only because structural NTTPs require it.

`N` includes the trailing NUL; `size()` / `view()` are `N-1`. The type is `fstr::fstr<N>` (namespace == type), not `fixed_string`.

Rejections that fail because of `consteval` (runtime buffer, concatenation, embedded NUL) live in `tests/compile_fail/`. Those TUs are `EXCLUDE_FROM_ALL`; ctest builds them with `WILL_FAIL TRUE`.
