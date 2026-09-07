#pragma once

#include "owl/extract/extractors.h"
#include "owl/http/response.h"
#include "prometheus/http.h"
#include "prometheus/registry.h"

namespace owl::prometheus {
    // Function pointer so owl::get(prometheus.handler) works; lambdas
    // cannot be route handlers.
    [[nodiscard]] inline owl::Response handler(owl::RequestView) {
        return owl::Response::body(dump(), "text/plain; version=0.0.4; charset=utf-8");
    }

    struct Prometheus final {
        owl::Response (*handler)(owl::RequestView);
    };

    [[nodiscard]] inline Prometheus make() noexcept {
        return {.handler = &handler};
    }
}
