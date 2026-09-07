#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <gtest/gtest.h>

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

TEST_F(PrometheusClear, RecordRequestWritesCounterAndHistogram) {
    owl::prometheus::record_request("GET", "/ping", 200, 0.5);
    const std::string body = owl::prometheus::dump();
    EXPECT_NE(body.find("http_requests_total{method=\"GET\",status=\"200\",route=\"/ping\"} 1"), std::string::npos);
    EXPECT_NE(body.find("# TYPE http_request_duration_seconds histogram"), std::string::npos);
    EXPECT_NE(body.find("http_request_duration_seconds_bucket{method=\"GET\",status=\"200\",route=\"/ping\",le=\"0.5\"} 1"), std::string::npos);
    EXPECT_NE(body.find("http_request_duration_seconds_sum{method=\"GET\",status=\"200\",route=\"/ping\"} 0.5"), std::string::npos);
    EXPECT_NE(body.find("http_request_duration_seconds_count{method=\"GET\",status=\"200\",route=\"/ping\"} 1"), std::string::npos);
}

TEST_F(PrometheusClear, RecordRequestAccumulatesPerSeries) {
    owl::prometheus::record_request("GET", "/ping", 200, 0.5);
    owl::prometheus::record_request("GET", "/ping", 200, 0.25);
    owl::prometheus::record_request("POST", "/ping", 201, 0.25);
    const std::string body = owl::prometheus::dump();
    EXPECT_NE(body.find("http_requests_total{method=\"GET\",status=\"200\",route=\"/ping\"} 2"), std::string::npos);
    EXPECT_NE(body.find("http_requests_total{method=\"POST\",status=\"201\",route=\"/ping\"} 1"), std::string::npos);
    EXPECT_NE(body.find("http_request_duration_seconds_sum{method=\"GET\",status=\"200\",route=\"/ping\"} 0.75"), std::string::npos);
    EXPECT_NE(body.find("http_request_duration_seconds_count{method=\"GET\",status=\"200\",route=\"/ping\"} 2"), std::string::npos);
}

TEST_F(PrometheusClear, RecordRequestAcceptsUnmatchedPattern) {
    // 404/405 never reach middleware; callers that want them counted pass
    // a pattern of their choosing.
    owl::prometheus::record_request("GET", "unmatched", 404, 0.0);
    const std::string body = owl::prometheus::dump();
    EXPECT_NE(body.find("http_requests_total{method=\"GET\",status=\"404\",route=\"unmatched\"} 1"), std::string::npos);
}

TEST_F(PrometheusClear, RecordUnmatchedCountsOnly) {
    owl::prometheus::record_unmatched("GET", "unmatched", 404);
    const std::string body = owl::prometheus::dump();
    EXPECT_NE(body.find("http_requests_total{method=\"GET\",status=\"404\",route=\"unmatched\"} 1"), std::string::npos);
    // No honest elapsed exists for an unmatched request, so no duration
    // series is created at all.
    EXPECT_EQ(body.find("http_request_duration_seconds"), std::string::npos);
}
