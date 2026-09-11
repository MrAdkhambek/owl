#pragma once

// What owl records about an HTTP exchange, in one place.
//
// Dispatch says where an exchange ends -- a handler's response, a throw
// unwound to 500, a route that never matched -- and this header says what
// to do about it. It is the only place on the request path that asks
// whether OWL_ENABLE_PROMETHEUS is on: dispatch calls the same members
// either way, and with the option off they are empty and the compiler
// drops the calls along with their arguments.

#include <string_view>

#include "owl/core/method.h"
#include "owl/http/request.h"

#ifdef OWL_ENABLE_PROMETHEUS
#include <chrono>

#include <prometheus/http.h>
#endif

namespace owl::detail {
#ifdef OWL_ENABLE_PROMETHEUS
    // One exchange, timed from construction. Built before the handler runs
    // so that a handler that throws is still charged the time it spent
    // unwinding.
    class Exchange final {
    public:
        // Reached a handler and ended with status: the handler's own, a
        // layer's that kicked, or the 500 an exception unwound to.
        void matched(const Request& request, const int status) const noexcept {
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
            prometheus::record_request(to_string(request.method()), request.route_pattern(), status, elapsed);
        }

        // Never reached a handler: 404/405, or dispatch itself failed.
        // Counted under the pattern but not timed, since there is no honest
        // elapsed to report.
        static void unmatched(const std::string_view method, const std::string_view pattern, const int status) noexcept {
            prometheus::record_unmatched(method, pattern, status);
        }

    private:
        std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    };
#else
    struct Exchange final {
        void matched(const Request&, int) const noexcept {
        }

        static void unmatched(std::string_view, std::string_view, int) noexcept {
        }
    };
#endif
}
