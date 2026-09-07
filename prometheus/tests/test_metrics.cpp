#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <owl/owl.h>
#include <prometheus/prometheus.h>

struct PrometheusClear : testing::Test {
    PrometheusClear() {
        owl::prometheus::clear();
    }
};

TEST_F(PrometheusClear, ConcurrentIncDuringDump) {
    const auto jobs = owl::prometheus::counter("jobs_total");
    std::thread producer([&] {
        for (auto i = 0; i < 10000; ++i) jobs.inc();
    });
    std::string body;
    for (auto i = 0; i < 100; ++i) body = owl::prometheus::dump();
    producer.join();
    EXPECT_NE(body.find("jobs_total"), std::string::npos);
}

TEST_F(PrometheusClear, RejectsInvalidMetricName) {
    EXPECT_THROW(owl::prometheus::counter("not a name").inc(), std::invalid_argument);
}

TEST_F(PrometheusClear, CounterAppearsInDump) {
    owl::prometheus::counter("jobs_total").inc();
    const std::string body = owl::prometheus::dump();
    EXPECT_NE(body.find("# TYPE jobs_total counter"), std::string::npos);
    EXPECT_NE(body.find("jobs_total 1"), std::string::npos);
}

TEST_F(PrometheusClear, LabeledCounterAppearsInDump) {
    owl::prometheus::counter("jobs_total", {"status"}).labels({"ok"}).inc();
    const std::string body = owl::prometheus::dump();
    EXPECT_NE(body.find("jobs_total{status=\"ok\"} 1"), std::string::npos);
}

TEST_F(PrometheusClear, EscapesLabelValues) {
    owl::prometheus::counter("jobs_total", {"path"}).labels({"a\"b\\c"}).inc();
    const std::string body = owl::prometheus::dump();
    EXPECT_NE(body.find("jobs_total{path=\"a\\\"b\\\\c\"} 1"), std::string::npos);
}

TEST_F(PrometheusClear, GaugeSetAppearsInDump) {
    owl::prometheus::gauge("queue_depth").set(3);
    const std::string body = owl::prometheus::dump();
    EXPECT_NE(body.find("# TYPE queue_depth gauge"), std::string::npos);
    EXPECT_NE(body.find("queue_depth 3"), std::string::npos);
}

TEST_F(PrometheusClear, HistogramObserveAppearsInDump) {
    owl::prometheus::histogram("work_seconds").observe(0.2);
    const std::string body = owl::prometheus::dump();
    EXPECT_NE(body.find("# TYPE work_seconds histogram"), std::string::npos);
    EXPECT_NE(body.find("work_seconds_bucket{le=\"0.1\"} 0"), std::string::npos);
    EXPECT_NE(body.find("work_seconds_bucket{le=\"0.25\"} 1"), std::string::npos);
    EXPECT_NE(body.find("work_seconds_bucket{le=\"+Inf\"} 1"), std::string::npos);
    EXPECT_NE(body.find("work_seconds_count 1"), std::string::npos);
}

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

    owl::Response boom() {
        throw std::runtime_error("boom");
    }

    struct App final {};

    template <typename S>
    owl::Response run_chain(owl::Router<S>& router, Fixture& fixture, const std::string_view path,
                            const owl::MiddlewareChain<S>* const server = nullptr,
                            const owl::Context<S>& ctx = {}) {
        owl::detail::MatchedChains<S> chains{};
        const auto* const handler = router.match(owl::Method::Get, path, *fixture.request, &chains);
        EXPECT_NE(handler, nullptr);
        owl::Terminal<S> term{handler};
        const auto [data, count] = chains.splice(server);
        const owl::Next<S> next{data, count, &term, &ctx};
        return coro::sync_wait(next(*fixture.request));
    }
}

TEST_F(PrometheusClear, MakeHandlerDumpsExposition) {
    owl::prometheus::counter("jobs_total").inc();
    const auto [handler] = owl::prometheus::make();
    Fixture fixture;
    fixture.send(handler(owl::RequestView{fixture.request}));
    EXPECT_EQ(fixture.req.res.status, 200);
    EXPECT_NE(fixture.capture.body.find("# TYPE jobs_total counter"), std::string::npos);
    EXPECT_NE(fixture.header("content-type").find("text/plain"), std::string::npos);
}

TEST_F(PrometheusClear, HttpLayerRecordsRoutePattern) {
    auto router = owl::Router<App>::make().route<"/ping">(owl::get(ping));
    owl::MiddlewareChain<App> server;
    server.emplace_back(owl::wrap_layer<App>(owl::prometheus::http));
    Fixture fixture;
    fixture.send(run_chain(router, fixture, "/ping", &server));
    const std::string body = owl::prometheus::dump();
    EXPECT_NE(body.find("http_requests_total{method=\"GET\",status=\"200\",route=\"/ping\"} 1"), std::string::npos);
}

TEST_F(PrometheusClear, RecordUnmatchedUsesUnmatchedRoute) {
    const Fixture fixture;
    owl::prometheus::record_unmatched(*fixture.request, 404);
    const std::string body = owl::prometheus::dump();
    EXPECT_NE(body.find("http_requests_total{method=\"GET\",status=\"404\",route=\"unmatched\"} 1"), std::string::npos);
    EXPECT_EQ(body.find("http_request_duration_seconds"), std::string::npos);
}

TEST_F(PrometheusClear, HttpLayerRecordsThrownHandlerAs500) {
    auto router = owl::Router<App>::make().route<"/boom">(owl::get(boom));
    owl::MiddlewareChain<App> server;
    server.emplace_back(owl::wrap_layer<App>(owl::prometheus::http));
    Fixture fixture;
    EXPECT_THROW(run_chain(router, fixture, "/boom", &server), std::runtime_error);
    const std::string body = owl::prometheus::dump();
    EXPECT_NE(body.find("http_requests_total{method=\"GET\",status=\"500\",route=\"/boom\"} 1"), std::string::npos);
    EXPECT_NE(body.find("http_request_duration_seconds_count{method=\"GET\",status=\"500\",route=\"/boom\"} 1"), std::string::npos);
}

TEST_F(PrometheusClear, AuthLayerKicksScrape) {
    const auto prometheus = owl::prometheus::make();
    const auto kick = [](const owl::Request&, auto) -> coro::task<owl::Response> {
        co_return owl::Response::ok("unauthorized", 401);
    };
    auto router = owl::Router<App>::make()
                      .layer(kick)
                      .route<"/metrics">(owl::get(prometheus.handler));
    Fixture fixture;
    fixture.send(run_chain(router, fixture, "/metrics"));
    EXPECT_EQ(fixture.req.res.status, 401);
    EXPECT_EQ(fixture.capture.body, "unauthorized");
    EXPECT_EQ(owl::prometheus::dump().find("# TYPE jobs_total"), std::string::npos);
}
