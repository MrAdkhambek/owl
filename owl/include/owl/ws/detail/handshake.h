#pragma once

// Which slot a GET takes: whether the client asked to become a WebSocket,
// and nothing more. The version and the key are judged by upgrade(), which
// can answer 426 or 400. Judging them here sent a version-8 client to the
// GET handler or a 404, where it could not learn to retry with 13.
//
// Reading `req->upgrade` costs one pointer test on every GET. Scanning
// headers would cost a full header scan instead.

#include <string_view>

#include "owl/http/request.h"
#include "owl/util/util.h"

namespace owl::ws::detail {
    [[nodiscard]] inline bool wants_websocket(const Request& req) {
        const auto* const raw = req.raw();
        return raw->upgrade.base != nullptr && util::eq_ci(std::string_view{raw->upgrade.base, raw->upgrade.len}, "websocket");
    }
}
