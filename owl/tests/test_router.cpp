#include <concepts>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
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

    struct Other {
        int n = 0;
    };

    template <typename Outer, typename Inner>
    concept can_nest = requires(Outer outer, Inner inner) {
        std::move(outer).template nest<"/api">(std::move(inner));
    };

    static_assert(can_nest<owl::Router<App>, owl::Router<App>>);
    static_assert(can_nest<owl::Router<>, owl::Router<>>);
    static_assert(!can_nest<owl::Router<App>, owl::Router<Other>>);
    static_assert(!can_nest<owl::Router<App>, owl::Router<>>);
    static_assert(!can_nest<owl::Router<>, owl::Router<App>>);

    owl::Response read_state(owl::State<App> app) {
        return owl::Response::ok(std::to_string(app->n));
    }

    owl::Response echo_id(owl::PathView<"id"> id) {
        return owl::Response::ok(std::string{id.value});
    }

    coro::task<owl::Response> pass_through(const owl::Request& req, owl::Next next) {
        co_return co_await next(req);
    }
}

TEST(Router, MatchesLiteralGet) {
    auto router = owl::Router<>::make()
                      .route<"/ping">(owl::get(ping));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::from(&req);
    const auto* handler = router.match(owl::Method::Get, "/ping", *request);
    ASSERT_NE(handler, nullptr);
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, UnknownPathIsNull) {
    auto router = owl::Router<>::make()
                      .route<"/ping">(owl::get(ping));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::from(&req);
    EXPECT_EQ(router.match(owl::Method::Get, "/nope", *request), nullptr);
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, CapturesPathParam) {
    auto router = owl::Router<>::make()
                      .route<"/users/{id}">(owl::get(echo_id));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::from(&req);
    ASSERT_NE(router.match(owl::Method::Get, "/users/42", *request), nullptr);
    EXPECT_EQ(request->param("id"), "42");
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, StateFromContext) {
    auto router = owl::Router<App>::make()
                      .route<"/n">(owl::get(read_state))
                      .with_state(std::make_shared<App>(App{.n = 9}));
    static_assert(std::is_same_v<decltype(router), owl::Router<>>,
                  "with_state must hand back a router that no longer needs state");
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::from(&req);
    const auto* handler = router.match(owl::Method::Get, "/n", *request);
    ASSERT_NE(handler, nullptr);
    const auto ctx = request->dispatch_context();
    ASSERT_NE(ctx.state, nullptr);
    ASSERT_TRUE(*ctx.state);
    EXPECT_EQ(std::static_pointer_cast<App>(*ctx.state)->n, 9);
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, WithStateRejectsNull) {
    EXPECT_THROW(
        (void)owl::Router<App>::make().route<"/n">(owl::get(read_state)).with_state(nullptr),
        std::invalid_argument);
}

TEST(Router, NestPrefix) {
    auto inner = owl::Router<>::make()
                     .route<"/ping">(owl::get(ping));
    auto router = owl::Router<>::make()
                      .nest<"/api/v1">(std::move(inner));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::from(&req);
    EXPECT_NE(router.match(owl::Method::Get, "/api/v1/ping", *request), nullptr);
    EXPECT_EQ(router.match(owl::Method::Get, "/ping", *request), nullptr);
    h2o_mem_clear_pool(&req.pool);
}

TEST(Router, NestUsesOuterState) {
    auto inner = owl::Router<App>::make()
                     .route<"/n">(owl::get(read_state));
    auto router = owl::Router<App>::make()
                      .nest<"/api">(std::move(inner))
                      .with_state(std::make_shared<App>(App{.n = 9}));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::from(&req);
    const auto* handler = router.match(owl::Method::Get, "/api/n", *request);
    ASSERT_NE(handler, nullptr);
    const auto ctx = request->dispatch_context();
    ASSERT_NE(ctx.state, nullptr);
    ASSERT_TRUE(*ctx.state);
    EXPECT_EQ(std::static_pointer_cast<App>(*ctx.state)->n, 9);
    h2o_mem_clear_pool(&req.pool);
}

// Both sides are Router<void> once state is applied, so the type system cannot
// tell a bound sub-app from a stateless one. Grafting would drop the inner
// state silently; refuse instead.
TEST(Router, NestAfterWithStateThrows) {
    auto bound = owl::Router<App>::make()
                     .route<"/n">(owl::get(read_state))
                     .with_state(std::make_shared<App>(App{.n = 9}));
    EXPECT_THROW(
        (void)owl::Router<>::make().nest<"/api">(std::move(bound)),
        std::invalid_argument);
}

TEST(Router, LayerCollectsChain) {
    auto router = owl::Router<>::make()
                      .layer(pass_through)
                      .route<"/ping">(owl::get(ping));
    h2o_req_t req{};
    h2o_mem_init_pool(&req.pool);
    req.query_at = SIZE_MAX;
    auto* request = owl::Request::from(&req);
    owl::detail::MatchedChains chains{};
    ASSERT_NE(router.match(owl::Method::Get, "/ping", *request, &chains), nullptr);
    EXPECT_EQ(chains.count, 1u);
    h2o_mem_clear_pool(&req.pool);
}
