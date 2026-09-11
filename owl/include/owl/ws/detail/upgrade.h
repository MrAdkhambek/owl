#pragma once

// How a route asks to be upgraded. Expressed as a Response so middleware
// still sees HTTP, and so the driver -- which holds h2o_req_t -- is the
// only thing that may touch the request after 101.

#include <memory>

#include <coro/task.h>

namespace owl::ws::detail {
    struct Session;
}

namespace owl::detail {
    struct WsUpgrade {
        WsUpgrade() = default;
        WsUpgrade(const WsUpgrade&) = delete;
        WsUpgrade& operator=(const WsUpgrade&) = delete;
        virtual ~WsUpgrade() = default;
        [[nodiscard]] virtual coro::task<void> make(ws::detail::Session* session) = 0;
    };

    using WsUpgradePtr = std::shared_ptr<WsUpgrade>;
}
