#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <owl/routing/router.h>

namespace {
    struct Capture final {
        h2o_ostream_t super{};
        std::string body{};

        static void on_send(h2o_ostream_t* const self, h2o_req_t*, h2o_sendvec_t* const bufs, const std::size_t bufcnt, const h2o_send_state_t) {
            auto* const capture = reinterpret_cast<Capture*>(self);
            for (std::size_t i = 0; i < bufcnt; ++i) {
                capture->body.append(bufs[i].raw, bufs[i].len);
            }
        }
    };

    struct Fixture final {
        h2o_req_t req{};
        Capture capture{};
        owl::Request* request = nullptr;
        coro::task<> sending{};

        Fixture() {
            h2o_mem_init_pool(&req.pool);
            req.method = {.base = const_cast<char*>("GET"), .len = 3};
            req.query_at = SIZE_MAX;
            req.version = 0x101;
            req.res.content_length = SIZE_MAX;
            capture.super.do_send = &Capture::on_send;
            req._ostr_top = &capture.super;
            request = owl::Request::from(&req);
        }

        ~Fixture() {
            h2o_mem_clear_pool(&req.pool);
        }

        void send(owl::Response response) {
            sending = std::move(response).send(&req);
            sending.start();
        }

        [[nodiscard]] std::string header(const std::string_view name) const {
            for (std::size_t i = 0; i < req.res.headers.size; ++i) {
                const auto& entry = req.res.headers.entries[i];
                const auto entry_name = std::string_view{entry.name->base, entry.name->len};
                if (owl::util::eq_ci(entry_name, name)) {
                    return {entry.value.base, entry.value.len};
                }
            }
            return {};
        }
    };

    owl::Response ping() {
        return owl::Response::ok("pong");
    }

    int handler_calls = 0;

    owl::Response counted() {
        ++handler_calls;
        return owl::Response::ok("ok");
    }

    struct App final {
        int n = 7;
    };

    coro::task<owl::Response> timing(const owl::Request& req, owl::Next<App> next) {
        const auto start = std::chrono::steady_clock::now();
        owl::Response res = co_await next(req);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        res.header("x-elapsed-ms", std::to_string(elapsed));
        co_return std::move(res);
    }

    coro::task<owl::Response> kick(const owl::Request&, owl::Next<App>) {
        co_return owl::Response::ok("unauthorized", 401);
    }

    coro::task<owl::Response> with_state(const owl::Request& req, const owl::Context<App>& ctx, owl::Next<App> next) {
        auto res = co_await next(req);
        res.header("x-n", std::to_string(ctx.state->n));
        co_return std::move(res);
    }

    template <typename S>
    owl::Response run_chain(owl::Router<S>& router, Fixture& fixture, const std::string_view path,
                            const owl::Context<S>& ctx = {},
                            const owl::MiddlewareChain<S>* const server = nullptr) {
        owl::detail::MatchedChains<S> chains{};
        const auto* const handler = router.match(owl::Method::Get, path, *fixture.request, &chains);
        EXPECT_NE(handler, nullptr);
        owl::Terminal<S> term{handler};
        const auto span = chains.splice(server);
        const owl::Next<S> next{span.data, span.count, &term, &ctx};
        return coro::sync_wait(next(*fixture.request));
    }
}

TEST(Middleware, HandlerRunsWithNoLayers) {
    auto router = owl::Router<App>::make().route<"/ping">(owl::get(ping));
    Fixture fixture;
    fixture.send(run_chain(router, fixture, "/ping"));
    EXPECT_EQ(fixture.capture.body, "pong");
    EXPECT_EQ(fixture.req.res.status, 200);
}

TEST(Middleware, TimingAddsElapsedHeader) {
    auto router = owl::Router<App>::make()
                      .layer(timing)
                      .route<"/ping">(owl::get(ping));
    Fixture fixture;
    fixture.send(run_chain(router, fixture, "/ping"));
    EXPECT_EQ(fixture.capture.body, "pong");
    EXPECT_FALSE(fixture.header("x-elapsed-ms").empty());
}

TEST(Middleware, KickSkipsTheHandler) {
    handler_calls = 0;
    auto router = owl::Router<App>::make()
                      .layer(kick)
                      .route<"/ping">(owl::get(counted));
    Fixture fixture;
    fixture.send(run_chain(router, fixture, "/ping"));
    EXPECT_EQ(handler_calls, 0);
    EXPECT_EQ(fixture.req.res.status, 401);
    EXPECT_EQ(fixture.capture.body, "unauthorized");
}

TEST(Middleware, InjectsRouterState) {
    auto router = owl::Router<App>::make()
                      .layer(with_state)
                      .route<"/ping">(owl::get(ping));
    Fixture fixture;
    const owl::Context<App> ctx{std::make_shared<App>(App{.n = 9})};
    fixture.send(run_chain(router, fixture, "/ping", ctx));
    EXPECT_EQ(fixture.header("x-n"), "9");
}

TEST(Middleware, NestedLayerRunsOnlyUnderPrefix) {
    std::vector<std::string> order;
    const auto tag = [&order](std::string name) {
        return [name = std::move(name), &order](const owl::Request& req, auto next) -> coro::task<owl::Response> {
            order.push_back(name + "-in");
            auto res = co_await next(req);
            order.push_back(name + "-out");
            co_return std::move(res);
        };
    };

    auto inner = owl::Router<App>::make()
                     .layer(tag("inner"))
                     .route<"/ping">(owl::get(ping));
    auto router = owl::Router<App>::make()
                      .layer(tag("outer"))
                      .nest<"/api">(std::move(inner))
                      .route<"/ping">(owl::get(ping));

    Fixture under;
    under.send(run_chain(router, under, "/api/ping"));
    EXPECT_EQ(order, (std::vector<std::string>{"outer-in", "inner-in", "inner-out", "outer-out"}));

    order.clear();
    Fixture root;
    root.send(run_chain(router, root, "/ping"));
    EXPECT_EQ(order, (std::vector<std::string>{"outer-in", "outer-out"}));
}

TEST(Middleware, ServerChainRunsBeforeRouter) {
    std::vector<std::string> order;
    const auto tag = [&order](std::string name) {
        return [name = std::move(name), &order](const owl::Request& req, auto next) -> coro::task<owl::Response> {
            order.push_back(name);
            co_return co_await next(req);
        };
    };

    auto router = owl::Router<App>::make()
                      .layer(tag("router"))
                      .route<"/ping">(owl::get(ping));
    owl::MiddlewareChain<App> server;
    server.emplace_back(owl::wrap_layer<App>(tag("server")));

    Fixture fixture;
    fixture.send(run_chain(router, fixture, "/ping", {}, &server));
    EXPECT_EQ(order, (std::vector<std::string>{"server", "router"}));
}
