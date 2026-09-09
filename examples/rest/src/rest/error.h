#pragma once

// Every failure a handler answers itself is {"error": "..."} plus the
// status that fits. The framework's own kicks -- a missing token, a body
// that is not the JSON asked for -- stay plain text, which is what a kick
// is.

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <owl/owl.h>

namespace rest {
    inline owl::Response fail(const int status, const std::string_view message) {
        return owl::Response::json(nlohmann::json{{"error", std::string{message}}}, status);
    }
}
