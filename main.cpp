#include <chrono>
#include <cstdio>
#include <format>
#include <memory>
#include <string>

#include <coro/executors/static_thread_pool.h>
#include <owl/coro/loop_scheduler.h>
#include <owl/routing/router.h>
#include <owl/server.h>

namespace {
    struct AppState {
        int hits = 0;
        coro::static_thread_pool pool{5};
    };

    owl::Response ping(owl::RequestView req) {
        return owl::Response::ok("pong");
    }

    owl::Response hello(owl::RequestView req, owl::PathView<"name"> name) {
        return owl::Response::ok(std::format("hello {}", name.value));
    }

    coro::task<owl::Response> calc(
        const owl::RequestView req,
        const owl::loop_scheduler loop,
        const owl::State<AppState> app,
        const owl::Path<"number", int> num
    ) {
        co_await app->pool.schedule();

        auto sum = 0;
        for (auto i = 1; i <= num.value; ++i) {
            sum += i;
        }

        co_await loop.schedule();
        co_return owl::Response::ok(std::format("calc {}", sum));
    }

    coro::task<owl::Response> hits(owl::RequestView req, const owl::State<AppState> app) {
        ++app->hits;
        const auto json = nlohmann::json({{"hits", app->hits}});
        co_return owl::Response::json(json, 200);
    }

    owl::StreamBody ticks(const owl::loop_scheduler loop) {
        for (auto i = 1; i <= 1000; ++i) {
            co_await loop.schedule_after(std::chrono::seconds{1});
            co_yield std::format("tick {}", i);
        }
    }

    owl::Response sse(owl::RequestView req, owl::loop_scheduler loop) {
        return owl::Response::sse(ticks(loop));
    }

    owl::StreamBody blob() {
        co_yield std::string{"hello"};
    }

    coro::task<owl::Response> download(owl::RequestView req) {
        co_return owl::Response::stream(blob(), "application/octet-stream");
    }

    coro::task<owl::Response> timing(const owl::Request& req, const owl::Next next) {
        const auto start = std::chrono::steady_clock::now();
        owl::Response res = co_await next(req);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        res.header("x-elapsed-ms", std::to_string(elapsed));
        co_return std::move(res);
    }
}

int main() {
    const auto state = std::make_shared<AppState>();

    auto v1 = owl::Router<AppState>::make()
              .route<"/ping">(owl::get(ping))
              .route<"/hits">(owl::get(hits));

    auto router = owl::Router<AppState>::make()
                  .layer(timing)
                  .nest<"/api/v1">(std::move(v1))
                  .route<"/ping">(owl::get(ping))
                  .route<"/hello/{name}">(owl::get(hello))
                  .route<"/calc/{number}">(owl::get(calc))
                  .route<"/hits">(owl::get(hits))
                  .route<"/sse">(owl::get(sse))
                  .route<"/stream">(owl::get(download))
                  .with_state(state);

    owl::Server server = owl::Server::builder()
                         .router(std::move(router))
                         .config({.port = 8080})
                         .thread(4)
                         .build();

    std::printf(
        "listening on http://127.0.0.1:%u\n  GET /ping\n  GET /hello/{name}\n  GET /calc/{number}\n  GET /hits\n  GET /sse\n  GET /stream\n  GET /api/v1/ping\n  GET /api/v1/hits\n",
        server.port());
    std::fflush(stdout);
    server.start();
}
