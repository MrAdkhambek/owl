#include <gtest/gtest.h>
#include <string>

#include <h2o.h>

#include <owl/http/request.h>
#include <owl/ws/detail/handshake.h>

namespace {
    void add_header(h2o_req_t& req, const char* const name, const char* const value) {
        h2o_add_header_by_str(&req.pool, &req.headers, name, std::char_traits<char>::length(name), 1, nullptr, value, std::char_traits<char>::length(value));
    }

    void set_upgrade(h2o_req_t& req) {
        req.upgrade = h2o_iovec_init(H2O_STRLIT("websocket"));
    }
}

TEST(WsHandshake, DetectsUpgradeVersion13AndKey) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    set_upgrade(req);
    add_header(req, "sec-websocket-version", "13");
    add_header(req, "sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ==");
    {
        const auto* const request = owl::Request::make(&req);
        EXPECT_TRUE(owl::ws::detail::is_websocket_handshake(*request));
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(WsHandshake, RejectsMissingKey) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    set_upgrade(req);
    add_header(req, "sec-websocket-version", "13");
    {
        const auto* const request = owl::Request::make(&req);
        EXPECT_FALSE(owl::ws::detail::is_websocket_handshake(*request));
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(WsHandshake, RejectsVersion8) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    set_upgrade(req);
    add_header(req, "sec-websocket-version", "8");
    add_header(req, "sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ==");
    {
        const auto* const request = owl::Request::make(&req);
        EXPECT_FALSE(owl::ws::detail::is_websocket_handshake(*request));
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(WsHandshake, RejectsPlainGet) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    {
        const auto* const request = owl::Request::make(&req);
        EXPECT_FALSE(owl::ws::detail::is_websocket_handshake(*request));
    }
    h2o_mem_clear_pool(&req.pool);
}

// h2o never leaves Upgrade in req->headers; a detector that looked there
// would pass hand-built tests and miss every live handshake.
TEST(WsHandshake, UpgradeHeaderAloneIsNotEnough) {
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    add_header(req, "upgrade", "websocket");
    add_header(req, "sec-websocket-version", "13");
    add_header(req, "sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ==");
    {
        const auto* const request = owl::Request::make(&req);
        EXPECT_FALSE(owl::ws::detail::is_websocket_handshake(*request));
    }
    h2o_mem_clear_pool(&req.pool);
}
