#pragma once

// HTTP RED on top of the registry. The caller owns the HTTP side -- where
// method/route/status come from, and whether unmatched requests are counted
// -- so this header knows nothing about any web framework.

#include <string>
#include <string_view>

#include "prometheus/registry.h"

namespace owl::prometheus {
    // Recording must never break the request path, so registry errors (name
    // and label violations) are swallowed rather than propagated.
    inline void record_request(
        const std::string_view method,
        const std::string_view pattern,
        const int status,
        const double elapsed
    ) noexcept {
        try {
            const auto code = std::to_string(status);
            counter("http_requests_total", {"method", "status", "route"}).labels({method, code, pattern}).inc();
            histogram("http_request_duration_seconds", {"method", "status", "route"}).labels({method, code, pattern}).observe(elapsed);
        } catch (...) {
        }
    }

    // Requests that never reached a handler -- 404/405, or a layer kicked
    // before the router. Count only: there is no honest elapsed to report,
    // and a fabricated zero sample would pull latency percentiles down.
    inline void record_unmatched(const std::string_view method, const std::string_view pattern, const int status) noexcept {
        try {
            const auto code = std::to_string(status);
            counter("http_requests_total", {"method", "status", "route"}).labels({method, code, pattern}).inc();
        } catch (...) {
        }
    }

    // The scrape body: Prometheus text 0.0.4. Serve it yourself with
    // Content-Type: text/plain; version=0.0.4; charset=utf-8.
    [[nodiscard]] static std::string dump() {
        return detail::registry().dump();
    }
}
