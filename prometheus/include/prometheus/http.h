#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include <coro/task.h>

#include "owl/core/method.h"
#include "owl/http/request.h"
#include "owl/http/response.h"
#include "owl/routing/middleware.h"
#include "prometheus/registry.h"

namespace owl::prometheus {
    inline void record_request(const owl::Request& req, const int status, const double elapsed) {
        try {
            const auto method = owl::to_string(req.method());
            const auto code = std::to_string(status);
            const auto route = req.route_pattern().empty() ? std::string_view{"unmatched"} : req.route_pattern();
            counter("http_requests_total", {"method", "status", "route"}).labels({method, code, route}).inc();
            histogram("http_request_duration_seconds", {"method", "status", "route"}).labels({method, code, route}).observe(elapsed);
        } catch (...) {
        }
    }

    // 404/405 never enter middleware. Count only — observe(0) would pull
    // latency percentiles toward zero on scans.
    inline void record_unmatched(const owl::Request& req, const int status) {
        try {
            const auto method = owl::to_string(req.method());
            const auto code = std::to_string(status);
            constexpr std::string_view route = "unmatched";
            counter("http_requests_total", {"method", "status", "route"}).labels({method, code, route}).inc();
        } catch (...) {
        }
    }

    inline constexpr auto http = [](const owl::Request& req, auto next) -> coro::task<owl::Response> {
        struct InFlight final {
            InFlight() { gauge("http_requests_in_flight").inc(); }
            ~InFlight() { gauge("http_requests_in_flight").dec(); }
        };
        const InFlight in_flight{};
        const auto start = std::chrono::steady_clock::now();
        try {
            owl::Response res = co_await next(req);
            record_request(req, res.status(), std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            co_return res;
        } catch (...) {
            record_request(req, 500, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            throw;
        }
    };
}
