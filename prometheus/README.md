<div align="center">

# prometheus

**Header-only Prometheus metrics for owl**

First-party counters, gauges, histograms, HTTP RED, and a scrape handler
you mount yourself.

[![C++](https://img.shields.io/badge/C%2B%2B-23-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/23)
[![CMake](https://img.shields.io/badge/target-owl::prometheus-064F8C?logo=cmake&logoColor=white)](#enable)
[![header-only](https://img.shields.io/badge/header--only-yes-success)](#)

[Enable](#enable) · [Scrape](#scrape) · [App metrics](#app-metrics) · [HTTP RED](#http-red) · [Layout](#layout)

</div>

> [!WARNING]
> Part of [owl](../README.md). Experimental. Not production-ready.

```cpp
#include <prometheus/prometheus.h>

struct App final {};

const auto prometheus = owl::prometheus::make();

auto admin = owl::Router<App>::make()
             .layer(basic_auth)
             .route<"/metrics">(owl::get(prometheus.handler));
```

Link `owl::prometheus` (pulls `owl::owl`). There is no automatic `/metrics`.
Auth is your middleware. The path is yours.

## Enable

Off unless the superproject is configured with the option **and** the app
links the target:

```cmake
cmake -S . -B build -DOWL_ENABLE_PROMETHEUS=ON
target_link_libraries(app PRIVATE owl::prometheus)
```

Include `<prometheus/prometheus.h>`. Mount RED yourself with
`.layer(owl::prometheus::http)`. `<owl/owl.h>` never pulls metrics.

## Scrape

`owl::prometheus::make()` returns a function pointer so `owl::get` can take
it. Lambdas cannot be route handlers.

```cpp
const auto prometheus = owl::prometheus::make();
router.route<"/metrics">(owl::get(prometheus.handler));
```

The handler dumps Prometheus text 0.0.4
(`Content-Type: text/plain; version=0.0.4; charset=utf-8`). Wrap the route
with whatever you already use for auth. Default listen is `127.0.0.1`.

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

Mount `.layer(owl::prometheus::http)` on the server (or a router). It records:

| Metric | Labels |
|---|---|
| `http_requests_total` | `method`, `status`, `route` |
| `http_request_duration_seconds` | `method`, `status`, `route` |
| `http_requests_in_flight` | none |

`route` is the registered pattern (`/hello/{name}`), not the request path.
404/405 never enter middleware. Call `record_unmatched` yourself if you want
them counted — do **not** observe duration (a zero sample would pull
percentiles down). A handler that throws is counted as `500` with elapsed
time, then rethrown.

## Layout

| Header | Role |
|---|---|
| `prometheus/prometheus.h` | `make()`, scrape `handler` |
| `prometheus/registry.h` | counter / gauge / histogram, dump |
| `prometheus/http.h` | RED middleware, unmatched |
