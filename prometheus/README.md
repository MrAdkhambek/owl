<div align="center">

# prometheus

**Header-only Prometheus metrics for C++23**

First-party counters, gauges, histograms, HTTP RED, and Prometheus
text-exposition dumps. Standalone — it depends on no web framework.

[![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/23)
[![CMake](https://img.shields.io/badge/target-owl::prometheus-064F8C?logo=cmake&logoColor=white)](#enable)
[![header-only](https://img.shields.io/badge/header--only-yes-success)](#)

[Enable](#enable) · [App metrics](#app-metrics) · [HTTP RED](#http-red) · [Layout](#layout)

</div>

> [!WARNING]
> Part of [owl](../README.md). Experimental. Not production-ready.

```cpp
#include <prometheus/prometheus.h>

owl::prometheus::counter("jobs_total").inc();
owl::prometheus::record_request("GET", "/ping", 200, 0.0142);
std::string body = owl::prometheus::dump();
```

Link `owl::prometheus`. Despite the target name it pulls nothing — no owl,
no libh2o — and knows nothing about `Request` or `Response`. `dump()` emits
Prometheus text 0.0.4 (`text/plain; version=0.0.4; charset=utf-8`).

## Enable

Off unless the superproject is configured with the option:

```cmake
cmake -S . -B build -DOWL_ENABLE_PROMETHEUS=ON
```

With the option ON, `owl::owl` depends on `owl::prometheus` and dispatch
records HTTP RED itself — you only serve `dump()`. Link
`owl::prometheus` directly when you use it without owl; then you record
yourself (see [HTTP RED](#http-red)). `<owl/owl.h>` never pulls metrics.

## App metrics

```cpp
owl::prometheus::counter("jobs_total").inc();
owl::prometheus::counter("jobs_total", {"status"}).labels({"ok"}).inc();
owl::prometheus::gauge("queue_depth").set(3);
owl::prometheus::histogram("work_seconds").observe(dt);
```

Counter, Gauge, Histogram. No Summary. Histogram buckets are the Prometheus
HTTP defaults (`0.005` … `10` + `Inf`). Negative histogram samples are
ignored; a counter must not decrease.

Values are atomics. The series map is an atomic copy-on-write snapshot, so
inc/scrape do not take a mutex. A mutex runs only when inserting a **new**
series. After warmup, workers only `fetch_add`.

Do not put unbounded ids in labels. The route template is safe; the raw
path is not.

## HTTP RED

When owl is built with the option ON, its dispatch calls these for you:
matched exchanges (including middleware kicks) through `record_request` with
the registered route pattern, thrown handlers as `500`, and `404`/`405` or
dispatch failures through `record_unmatched`. Without owl, call them from
wherever you see a finished exchange (middleware, a dispatch hook, a proxy):

```cpp
owl::prometheus::record_request(method, route_pattern, status, elapsed_seconds);
```

It records:

| Metric | Labels |
|---|---|
| `http_requests_total` | `method`, `status`, `route` |
| `http_request_duration_seconds` | `method`, `status`, `route` |

`route` should be the registered pattern (`/hello/{name}`), not the request
path. For requests that never reach that layer (404/405, or a kick before
the router), call:

```cpp
owl::prometheus::record_unmatched(method, pattern, status);
```

It counts only and creates no duration series — there is no honest elapsed
to report, and a fabricated zero sample would pull percentiles down.

## Layout

| Header | Role |
|---|---|
| `prometheus/prometheus.h` | umbrella |
| `prometheus/registry.h` | counter / gauge / histogram, clear |
| `prometheus/http.h` | `record_request`, `record_unmatched`, `dump` |
