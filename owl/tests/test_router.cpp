#include <concepts>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <utility>

#include <owl/routing/router.h>
#include <owl/core/state.h>

namespace {
    owl::Response ping() {
        return owl::Response::ok("pong");
    }

    struct App {
        int n = 7;
    };

    template <typename Outer, typename Inner>
    concept can_nest = requires(Outer outer, Inner inner) {
        std::move(outer).template nest<"/api">(std::move(inner));
    };

    static_assert(can_nest<owl::Router<App>, owl::Router<App>>);

    owl::Response read_state(owl::State<App> app) {
        return owl::Response::ok(std::to_string(app->n));
    }

    owl::Response echo_id(owl::PathView<"id"> id) {
        return owl::Response::ok(std::string{id.value});
    }

    coro::task<owl::Response> pass_through(const owl::Request& req, owl::Next<App> next) {
        co_return co_await next(req);
    }
}

TEST(Router, MatchesLiteralGet) {
    auto router = owl::Router<App>::make()
                      .route<"/ping">(owl::get(ping));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    const auto* handler = router.match(owl::Method::Get, "/ping", *request);
    ASSERT_NE(handler, nullptr);
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, MatchSetsRoutePattern) {
    auto router = owl::Router<App>::make()
                      .route<"/ping">(owl::get(ping));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    ASSERT_NE(router.match(owl::Method::Get, "/ping", *request), nullptr);
    EXPECT_EQ(request->route_pattern(), "/ping");
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, UnknownPathIsNull) {
    auto router = owl::Router<App>::make()
                      .route<"/ping">(owl::get(ping));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    EXPECT_EQ(router.match(owl::Method::Get, "/nope", *request), nullptr);
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, CapturesPathParam) {
    auto router = owl::Router<App>::make()
                      .route<"/users/{id}">(owl::get(echo_id));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    ASSERT_NE(router.match(owl::Method::Get, "/users/42", *request), nullptr);
    EXPECT_EQ(request->param("id"), "42");
    EXPECT_EQ(request->route_pattern(), "/users/{id}");
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, RoutesStateHandler) {
    auto router = owl::Router<App>::make()
                      .route<"/n">(owl::get(read_state));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    EXPECT_NE(router.match(owl::Method::Get, "/n", *request), nullptr);
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, NestPrefix) {
    auto inner = owl::Router<App>::make()
                     .route<"/ping">(owl::get(ping));
    auto router = owl::Router<App>::make()
                      .nest<"/api/v1">(std::move(inner));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    EXPECT_NE(router.match(owl::Method::Get, "/api/v1/ping", *request), nullptr);
    EXPECT_EQ(router.match(owl::Method::Get, "/ping", *request), nullptr);
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, NestedMatchSetsFullRoutePattern) {
    auto inner = owl::Router<App>::make()
                     .route<"/ping">(owl::get(ping));
    auto router = owl::Router<App>::make()
                      .nest<"/api/v1">(std::move(inner));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    ASSERT_NE(router.match(owl::Method::Get, "/api/v1/ping", *request), nullptr);
    EXPECT_EQ(request->route_pattern(), "/api/v1/ping");
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, NestStateHandler) {
    auto inner = owl::Router<App>::make()
                     .route<"/n">(owl::get(read_state));
    auto router = owl::Router<App>::make()
                      .nest<"/api">(std::move(inner));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    EXPECT_NE(router.match(owl::Method::Get, "/api/n", *request), nullptr);
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, LayerCollectsChain) {
    auto router = owl::Router<App>::make()
                      .layer(pass_through)
                      .route<"/ping">(owl::get(ping));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::make(&req);
    owl::detail::MatchedChains<App> chains{};
    ASSERT_NE(router.match(owl::Method::Get, "/ping", *request, &chains), nullptr);
    EXPECT_EQ(chains.count, 1u);
    h2o_mem_clear_pool(&req.pool);
}
