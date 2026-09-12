#pragma once

// Match only needs to know the request looks like a handshake so it can pick
// the WS slot. Key validity and the accept key belong to `upgrade()`, which
// still answers 400.
//
// Reading `req->upgrade` costs one pointer test on every GET. Scanning
// headers would cost a full header scan instead.

#include <string_view>

#include "owl/http/request.h"
#include "owl/util/util.h"

namespace owl::ws::detail {
    [[nodiscard]] inline bool is_websocket_handshake(const Request& req) {
        const auto* const raw = req.raw();
        if (raw->upgrade.base == nullptr || !util::eq_ci(std::string_view{raw->upgrade.base, raw->upgrade.len}, "websocket")) {
            return false;
        }
        if (req.header("Sec-WebSocket-Version") != "13") {
            return false;
        }
        const auto key = req.header("Sec-WebSocket-Key");
        return key.has_value() && !key->empty();
    }
}
