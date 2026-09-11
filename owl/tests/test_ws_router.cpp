#include <gtest/gtest.h>
#include <stdexcept>
#include <string>

#include <h2o.h>

#include <coro/task.h>

#include <owl/core/state.h>
#include <owl/coro/loop_scheduler.h>
#include <owl/extract/extractors.h>
#include <owl/http/request.h>
#include <owl/http/response.h>
#include <owl/routing/router.h>
#include <owl/ws/detail/handshake.h>
#include <owl/ws/socket.h>

namespace {
    struct App {
        int n = 7;
    };

    void add_header(h2o_req_t& req, const char* const name, const char* const value) {
        h2o_add_header_by_str(&req.pool, &req.headers, name, std::char_traits<char>::length(name), 1, nullptr, value, std::char_traits<char>::length(value));
    }

    void set_upgrade(h2o_req_t& req) {
        req.upgrade = h2o_iovec_init(H2O_STRLIT("websocket"));
    }

    void set_handshake(h2o_req_t& req) {
        set_upgrade(req);
        add_header(req, "sec-websocket-version", "13");
        add_header(req, "sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ==");
    }

    coro::task<void> room(owl::ws::Socket, owl::Path<"id", int>) { co_return; }
    coro::task<void> chat(owl::ws::Socket) { co_return; }
    coro::task<void> looped(owl::ws::Socket, const owl::loop_scheduler&) { co_return; }
    owl::Response page() { return owl::Response::ok("html"); }
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

TEST(WsRouter, CoroutineFormMatchesOnlyOnUpgrade) {
    auto router = owl::Router<App>::make().ws<"/rooms/{id}">(room);
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    {
        auto* request = owl::Request::make(&req);
        EXPECT_EQ(router.match(owl::Method::Get, "/rooms/1", *request), nullptr);
    }
    set_handshake(req);
    {
        auto* request = owl::Request::make(&req);
        ASSERT_NE(router.match(owl::Method::Get, "/rooms/1", *request), nullptr);
        EXPECT_EQ(request->param("id"), "1");
    }
    h2o_mem_clear_pool(&req.pool);
}

TEST(WsRouter, GetAndWsShareAPath) {
    auto router = owl::Router<App>::make()
        .route<"/chat">(owl::get(page))
        .ws<"/chat">(chat);
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    const auto* const get_handler = router.match(owl::Method::Get, "/chat", *request);
    ASSERT_NE(get_handler, nullptr);
    set_handshake(req);
    const auto* const ws_handler = router.match(owl::Method::Get, "/chat", *request);
    ASSERT_NE(ws_handler, nullptr);
    EXPECT_NE(get_handler, ws_handler);
    h2o_mem_clear_pool(&req.pool);
}

TEST(WsRouter, WsOnlyPathAllowsNoMethod) {
    auto router = owl::Router<App>::make().ws<"/chat">(chat);
    EXPECT_TRUE(router.allowed_methods("/chat").empty());
}

TEST(WsRouter, LiteralGetBeatsWsParameter) {
    auto router = owl::Router<App>::make()
        .route<"/rooms/new">(owl::get(page))
        .ws<"/rooms/{id}">(room);
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    const auto* const get_handler = router.match(owl::Method::Get, "/rooms/new", *request);
    ASSERT_NE(get_handler, nullptr);
    set_handshake(req);
    const auto* const handshake_handler = router.match(owl::Method::Get, "/rooms/new", *request);
    EXPECT_EQ(handshake_handler, get_handler);
    h2o_mem_clear_pool(&req.pool);
}

TEST(WsRouter, AcceptsContextReferences) {
    auto router = owl::Router<App>::make().ws<"/loop">(looped);
    EXPECT_EQ(router.size(), 1u);
}

TEST(WsRouter, DuplicateWsThrows) {
    EXPECT_THROW(
        (void)owl::Router<App>::make().ws<"/x">(chat).ws<"/x">(chat),
        std::invalid_argument);
}

TEST(WsRouter, NestedWsCollides) {
    EXPECT_THROW(
        (void)owl::Router<App>::make().ws<"/x/y">(chat)
            .nest<"/x">(owl::Router<App>::make().ws<"/y">(chat)),
        std::invalid_argument);
}
