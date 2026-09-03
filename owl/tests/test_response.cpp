#include <chrono>
#include <cstddef>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <coro/async_generator.h>
#include <coro/task.h>

#include <owl/http/request.h>
#include <owl/http/response.h>

namespace {
    struct Capture final {
        h2o_ostream_t super{};
        std::string body{};
        bool final_state{};
        std::size_t sends{};
        std::vector<bool> in_progress{};

        static void on_send(h2o_ostream_t* const self, h2o_req_t*, h2o_sendvec_t* const bufs, const std::size_t bufcnt, const h2o_send_state_t state) {
            auto* const capture = reinterpret_cast<Capture*>(self);
            ++capture->sends;
            capture->in_progress.push_back(h2o_send_state_is_in_progress(state));
            capture->final_state = !h2o_send_state_is_in_progress(state);
            for (std::size_t i = 0; i < bufcnt; ++i) {
                capture->body.append(bufs[i].raw, bufs[i].len);
            }
        }
    };

    struct Fixture final {
        h2o_req_t req{};
        Capture capture{};
        coro::task<> sending{};

        Fixture() {
            h2o_mem_init_pool(&req.pool);
            req.method = {.base = const_cast<char*>("GET"), .len = 3};
            req.query_at = SIZE_MAX;
            req.version = 0x101;
            req.res.content_length = SIZE_MAX;
            capture.super.do_send = &Capture::on_send;
            req._ostr_top = &capture.super;
        }

        ~Fixture() {
            h2o_mem_clear_pool(&req.pool);
        }

        [[nodiscard]] bool streaming() const noexcept {
            return req._generator != nullptr;
        }

        void run(coro::task<> task) {
            sending = std::move(task);
            sending.start();
        }

        void proceed() {
            ASSERT_NE(req._generator, nullptr);
            req._generator->proceed(req._generator, &req);
        }

        void stop() {
            ASSERT_NE(req._generator, nullptr);
            req._generator->stop(req._generator, &req);
        }

        void drain(const std::size_t limit = 32) {
            for (std::size_t i = 0; i < limit && streaming(); ++i) {
                proceed();
            }
        }

        [[nodiscard]] std::vector<std::string> values(const std::string_view name) const {
            std::vector<std::string> found;
            for (std::size_t i = 0; i < req.res.headers.size; ++i) {
                const auto& entry = req.res.headers.entries[i];
                const auto entry_name = std::string_view{entry.name->base, entry.name->len};
                if (owl::util::eq_ci(entry_name, name)) {
                    found.emplace_back(entry.value.base, entry.value.len);
                }
            }
            return found;
        }

        [[nodiscard]] std::string orig_name(const std::string_view name) const {
            for (std::size_t i = 0; i < req.res.headers.size; ++i) {
                const auto& entry = req.res.headers.entries[i];
                const auto entry_name = std::string_view{entry.name->base, entry.name->len};
                if (owl::util::eq_ci(entry_name, name) && entry.orig_name != nullptr) {
                    return std::string{entry.orig_name, entry.name->len};
                }
            }
            return {};
        }

        [[nodiscard]] std::size_t count(const std::string_view name) const {
            return values(name).size();
        }

        [[nodiscard]] std::string value(const std::string_view name) const {
            const auto found = values(name);
            return found.empty() ? std::string{} : found.front();
        }
    };

    owl::StreamBody from_chunks(std::vector<std::string> chunks) {
        for (auto& chunk : chunks) {
            co_yield std::move(chunk);
        }
    }

    owl::StreamBody no_chunks() {
        co_return;
    }
}

TEST(Response, SetsStatusAndReason) {
    Fixture fixture;
    fixture.run(owl::Response::ok("made", 201).send(&fixture.req));
    EXPECT_EQ(fixture.req.res.status, 201);
    EXPECT_STREQ(fixture.req.res.reason, "Created");
}

TEST(Response, StatusIsReadableAfterBuild) {
    const auto response = owl::Response::ok("made", 201);
    EXPECT_EQ(response.status(), 201);
}

TEST(Response, MapsUnknownStatusToUnknownReason) {
    Fixture fixture;
    fixture.run(owl::Response::ok("x", 599).send(&fixture.req));
    EXPECT_STREQ(fixture.req.res.reason, "Unknown");
}

TEST(Response, DefaultsToStatusOk) {
    Fixture fixture;
    fixture.run(owl::Response::ok("hi").send(&fixture.req));
    EXPECT_EQ(fixture.req.res.status, 200);
    EXPECT_STREQ(fixture.req.res.reason, "OK");
}

TEST(Response, SendsBodyBytes) {
    Fixture fixture;
    fixture.run(owl::Response::ok("hello world").send(&fixture.req));
    EXPECT_EQ(fixture.capture.body, "hello world");
    EXPECT_TRUE(fixture.capture.final_state);
}

TEST(Response, SetsContentLengthWhenBodyPresent) {
    Fixture fixture;
    fixture.run(owl::Response::ok("hello").send(&fixture.req));
    EXPECT_EQ(fixture.req.res.content_length, 5u);
}

TEST(Response, TextUsesPlainContentType) {
    Fixture fixture;
    fixture.run(owl::Response::ok("hi").send(&fixture.req));
    EXPECT_EQ(fixture.value("content-type"), "text/plain; charset=utf-8");
}

TEST(Response, JsonUsesJsonContentType) {
    Fixture fixture;
    fixture.run(owl::Response::json(R"({"a":1})").send(&fixture.req));
    EXPECT_EQ(fixture.value("content-type"), "application/json");
    EXPECT_EQ(fixture.capture.body, R"({"a":1})");
}

TEST(Response, BodyAcceptsExplicitContentType) {
    Fixture fixture;
    fixture.run(owl::Response::body("x,y", "text/csv").send(&fixture.req));
    EXPECT_EQ(fixture.value("content-type"), "text/csv");
    EXPECT_EQ(fixture.capture.body, "x,y");
}

TEST(Response, HeaderIsEmitted) {
    Fixture fixture;
    fixture.run(owl::Response::ok("hi").header("x-trace", "abc123").send(&fixture.req));
    EXPECT_EQ(fixture.value("x-trace"), "abc123");
}

TEST(Response, MultipleDistinctHeadersAreEmitted) {
    Fixture fixture;
    fixture.run(owl::Response::ok("hi").header("x-one", "1").header("x-two", "2").send(&fixture.req));
    EXPECT_EQ(fixture.value("x-one"), "1");
    EXPECT_EQ(fixture.value("x-two"), "2");
}

TEST(Response, HeaderSurvivesTemporaryArguments) {
    Fixture fixture;
    {
        const auto name = std::string{"x-"} + "temp";
        const auto value = std::string{"val"} + "ue";
        fixture.run(owl::Response::ok("hi").header(name, value).send(&fixture.req));
    }
    EXPECT_EQ(fixture.value("x-temp"), "value");
}

TEST(Response, BodySurvivesTemporaryArgument) {
    Fixture fixture;
    {
        const auto body = std::string{"tem"} + "porary";
        fixture.run(owl::Response::ok(body).send(&fixture.req));
    }
    EXPECT_EQ(fixture.capture.body, "temporary");
}

TEST(Response, TwoSetCookieHeadersBothSurvive) {
    Fixture fixture;
    fixture.run(owl::Response::ok("hi")
           .set_cookie({.name = "a", .value = "1"})
           .set_cookie({.name = "b", .value = "2"})
           .send(&fixture.req));
    const auto cookies = fixture.values("set-cookie");
    ASSERT_EQ(cookies.size(), 2u);
    EXPECT_EQ(cookies[0], "a=1");
    EXPECT_EQ(cookies[1], "b=2");
}

TEST(Response, CookieSerializesAttributes) {
    Fixture fixture;
    fixture.run(owl::Response::ok("hi")
           .set_cookie({
               .name = "sid",
               .value = "abc",
               .path = "/",
               .domain = "example.com",
               .max_age = std::chrono::seconds{60},
               .http_only = true,
               .secure = true,
               .same_site = owl::same_site::lax,
           })
           .send(&fixture.req));
    EXPECT_EQ(fixture.value("set-cookie"),
              "sid=abc; Path=/; Domain=example.com; Max-Age=60; HttpOnly; Secure; SameSite=Lax");
}

TEST(Response, CookieSurvivesTemporaryArguments) {
    Fixture fixture;
    {
        const auto value = std::string{"ab"} + "c";
        fixture.run(owl::Response::ok("hi").set_cookie({.name = "sid", .value = value}).send(&fixture.req));
    }
    EXPECT_EQ(fixture.value("set-cookie"), "sid=abc");
}

TEST(Response, RedirectSetsLocationAndDefaultStatus) {
    Fixture fixture;
    fixture.run(owl::Response::redirect("/login").send(&fixture.req));
    EXPECT_EQ(fixture.req.res.status, 302);
    EXPECT_EQ(fixture.value("location"), "/login");
}

TEST(Response, RedirectAcceptsExplicitStatus) {
    Fixture fixture;
    fixture.run(owl::Response::redirect("/new", 301).send(&fixture.req));
    EXPECT_EQ(fixture.req.res.status, 301);
    EXPECT_STREQ(fixture.req.res.reason, "Unknown");
    EXPECT_EQ(fixture.value("location"), "/new");
}

TEST(Response, ErrorUsesCannedReasonBody) {
    Fixture fixture;
    fixture.run(owl::Response::error(404).send(&fixture.req));
    EXPECT_EQ(fixture.req.res.status, 404);
    EXPECT_STREQ(fixture.req.res.reason, "Not Found");
    EXPECT_EQ(fixture.capture.body, "not found");
}

TEST(Response, NoContentLeavesContentLengthUnset) {
    Fixture fixture;
    fixture.run(owl::Response::no_content().send(&fixture.req));
    EXPECT_EQ(fixture.req.res.status, 204);
    EXPECT_STREQ(fixture.req.res.reason, "No Content");
    EXPECT_EQ(fixture.capture.body, "");
    EXPECT_EQ(fixture.req.res.content_length, SIZE_MAX);
}

TEST(Response, ChainForwardsValueCategory) {
    Fixture fixture;
    auto named = owl::Response::ok("hi");
    static_assert(std::is_same_v<decltype(named.header("x", "y")), owl::Response&>);
    static_assert(!std::is_copy_constructible_v<owl::Response>);
    static_assert(std::is_same_v<decltype(owl::Response::ok("hi").header("x", "y")), owl::Response&&>);
    fixture.run(std::move(named).send(&fixture.req));
    EXPECT_EQ(fixture.capture.body, "hi");
}

TEST(Response, HeaderNameIsStoredLowercased) {
    Fixture fixture;
    fixture.run(owl::Response::ok("hi").header("X-Mixed-Case", "v").send(&fixture.req));
    EXPECT_EQ(fixture.value("x-mixed-case"), "v");
}

TEST(Response, HeaderPreservesOriginalNameCasing) {
    Fixture fixture;
    fixture.run(owl::Response::ok("hi").header("X-Mixed-Case", "v").send(&fixture.req));
    EXPECT_EQ(fixture.orig_name("x-mixed-case"), "X-Mixed-Case");
}

TEST(Response, RepeatedHeaderNameLastWins) {
    Fixture fixture;
    fixture.run(owl::Response::ok("hi").header("x-multi", "1").header("x-multi", "2").send(&fixture.req));
    const auto found = fixture.values("x-multi");
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0], "2");
}

TEST(Response, EmptyHeaderValueIsEmitted) {
    Fixture fixture;
    fixture.run(owl::Response::ok("hi").header("x-empty", "").send(&fixture.req));
    ASSERT_EQ(fixture.count("x-empty"), 1u);
    EXPECT_EQ(fixture.value("x-empty"), "");
}

TEST(Response, EmptyBodySetsZeroContentLength) {
    Fixture fixture;
    fixture.run(owl::Response::ok("").send(&fixture.req));
    EXPECT_EQ(fixture.req.res.content_length, 0u);
    EXPECT_EQ(fixture.capture.body, "");
}

TEST(Response, ErrorMapsMethodNotAllowed) {
    Fixture fixture;
    fixture.run(owl::Response::error(405).send(&fixture.req));
    EXPECT_EQ(fixture.req.res.status, 405);
    EXPECT_STREQ(fixture.req.res.reason, "Method Not Allowed");
    EXPECT_EQ(fixture.capture.body, "method not allowed");
}

TEST(Response, ErrorFallsBackToGenericBody) {
    Fixture fixture;
    fixture.run(owl::Response::error(500).send(&fixture.req));
    EXPECT_EQ(fixture.req.res.status, 500);
    EXPECT_STREQ(fixture.req.res.reason, "Internal Server Error");
    EXPECT_EQ(fixture.capture.body, "error");
}

TEST(Response, StatusAppliesWhenSetAfterBody) {
    Fixture fixture;
    fixture.run(owl::Response::json("{}", 201).send(&fixture.req));
    EXPECT_EQ(fixture.req.res.status, 201);
    EXPECT_STREQ(fixture.req.res.reason, "Created");
    EXPECT_EQ(fixture.capture.body, "{}");
}

TEST(Response, LastBodySetterWinsWithSingleContentType) {
    Fixture fixture;
    fixture.run(owl::Response::json(R"({"a":1})").send(&fixture.req));
    EXPECT_EQ(fixture.capture.body, R"({"a":1})");
    EXPECT_EQ(fixture.req.res.content_length, 7u);
    ASSERT_EQ(fixture.count("content-type"), 1u);
    EXPECT_EQ(fixture.value("content-type"), "application/json");
}

TEST(Response, RedirectDoesNotAccumulateLocationHeaders) {
    Fixture fixture;
    fixture.run(owl::Response::redirect("/second").send(&fixture.req));
    ASSERT_EQ(fixture.count("location"), 1u);
    EXPECT_EQ(fixture.value("location"), "/second");
}

TEST(Response, RedirectSetsZeroContentLength) {
    Fixture fixture;
    fixture.run(owl::Response::redirect("/login").send(&fixture.req));
    EXPECT_EQ(fixture.req.res.content_length, 0u);
    EXPECT_EQ(fixture.capture.body, "");
}

TEST(Response, NoContentAfterBodyDropsTheBody) {
    Fixture fixture;
    fixture.run(owl::Response::no_content().send(&fixture.req));
    EXPECT_EQ(fixture.req.res.status, 204);
    EXPECT_EQ(fixture.capture.body, "");
    EXPECT_EQ(fixture.req.res.content_length, SIZE_MAX);
}

TEST(Response, BodyMayContainNulBytes) {
    Fixture fixture;
    const auto payload = std::string{"a\0b", 3};
    fixture.run(owl::Response::body(payload, "application/octet-stream").send(&fixture.req));
    EXPECT_EQ(fixture.req.res.content_length, 3u);
    ASSERT_EQ(fixture.capture.body.size(), 3u);
    EXPECT_EQ(fixture.capture.body, payload);
}

TEST(Response, LargeBodyIsSentIntact) {
    Fixture fixture;
    const auto payload = std::string(64 * 1024, 'x');
    fixture.run(owl::Response::ok(payload).send(&fixture.req));
    EXPECT_EQ(fixture.req.res.content_length, payload.size());
    EXPECT_EQ(fixture.capture.body, payload);
}

TEST(Response, StreamSendsAllChunksInOrder) {
    Fixture fixture;
    fixture.run(owl::Response::stream(from_chunks({"one", "two", "three"}), "text/plain").send(&fixture.req));
    fixture.drain();
    EXPECT_EQ(fixture.capture.body, "onetwothree");
}

TEST(Response, StreamClosesAfterLastChunk) {
    Fixture fixture;
    fixture.run(owl::Response::stream(from_chunks({"a", "b"}), "text/plain").send(&fixture.req));
    fixture.drain();
    EXPECT_FALSE(fixture.streaming());
    EXPECT_TRUE(fixture.capture.final_state);
}

TEST(Response, StreamSendsChunksIncrementally) {
    Fixture fixture;
    fixture.run(owl::Response::stream(from_chunks({"a", "b"}), "text/plain").send(&fixture.req));
    EXPECT_EQ(fixture.capture.body, "a");
    fixture.proceed();
    EXPECT_EQ(fixture.capture.body, "ab");
    fixture.drain();
    EXPECT_EQ(fixture.capture.body, "ab");
}

TEST(Response, StreamMarksOnlyTheLastSendFinal) {
    Fixture fixture;
    fixture.run(owl::Response::stream(from_chunks({"a", "b"}), "text/plain").send(&fixture.req));
    fixture.drain();
    ASSERT_GE(fixture.capture.in_progress.size(), 2u);
    for (std::size_t i = 0; i + 1 < fixture.capture.in_progress.size(); ++i) {
        EXPECT_TRUE(fixture.capture.in_progress[i]) << "send " << i << " should be in progress";
    }
    EXPECT_FALSE(fixture.capture.in_progress.back());
}

TEST(Response, StreamLeavesContentLengthUnset) {
    Fixture fixture;
    fixture.run(owl::Response::stream(from_chunks({"abc"}), "text/plain").send(&fixture.req));
    fixture.drain();
    EXPECT_EQ(fixture.req.res.content_length, SIZE_MAX);
}

TEST(Response, StreamUsesGivenContentType) {
    Fixture fixture;
    fixture.run(owl::Response::stream(from_chunks({"x"}), "application/x-ndjson").send(&fixture.req));
    fixture.drain();
    EXPECT_EQ(fixture.value("content-type"), "application/x-ndjson");
}

TEST(Response, StreamAppliesStatus) {
    Fixture fixture;
    fixture.run(owl::Response::stream(from_chunks({"x"}), "text/plain", 201).send(&fixture.req));
    fixture.drain();
    EXPECT_EQ(fixture.req.res.status, 201);
    EXPECT_STREQ(fixture.req.res.reason, "Created");
}

TEST(Response, EmptyStreamClosesWithoutBody) {
    Fixture fixture;
    fixture.run(owl::Response::stream(no_chunks(), "text/plain").send(&fixture.req));
    fixture.drain();
    EXPECT_EQ(fixture.capture.body, "");
    EXPECT_FALSE(fixture.streaming());
    EXPECT_TRUE(fixture.capture.final_state);
}

TEST(Response, StreamSkipsEmptyChunks) {
    Fixture fixture;
    fixture.run(owl::Response::stream(from_chunks({"a", "", "b"}), "text/plain").send(&fixture.req));
    fixture.drain();
    EXPECT_EQ(fixture.capture.body, "ab");
}

TEST(Response, StopHaltsStreaming) {
    Fixture fixture;
    fixture.run(owl::Response::stream(from_chunks({"a", "b", "c"}), "text/plain").send(&fixture.req));
    EXPECT_EQ(fixture.capture.body, "a");
    fixture.stop();
    const auto after_stop = fixture.capture.body;
    EXPECT_EQ(after_stop, "a");
}

TEST(Response, SseSetsEventStreamContentType) {
    Fixture fixture;
    fixture.run(owl::Response::sse(from_chunks({"hi"})).send(&fixture.req));
    fixture.drain();
    EXPECT_EQ(fixture.value("content-type"), "text/event-stream");
}

TEST(Response, SseSetsCacheControlNoCache) {
    Fixture fixture;
    fixture.run(owl::Response::sse(from_chunks({"hi"})).send(&fixture.req));
    fixture.drain();
    EXPECT_EQ(fixture.value("cache-control"), "no-cache");
}

TEST(Response, SseDoesNotSetAConnectionHeader) {
    Fixture fixture;
    fixture.run(owl::Response::sse(from_chunks({"hi"})).send(&fixture.req));
    fixture.drain();
    EXPECT_TRUE(fixture.values("connection").empty());
}

TEST(Response, SseFramesSingleLinePayload) {
    Fixture fixture;
    fixture.run(owl::Response::sse(from_chunks({"hello"})).send(&fixture.req));
    fixture.drain();
    EXPECT_EQ(fixture.capture.body, "data: hello\n\n");
}

TEST(Response, SseFramesEachChunkSeparately) {
    Fixture fixture;
    fixture.run(owl::Response::sse(from_chunks({"one", "two"})).send(&fixture.req));
    fixture.drain();
    EXPECT_EQ(fixture.capture.body, "data: one\n\ndata: two\n\n");
}

TEST(Response, SseSplitsEmbeddedNewlinesIntoDataLines) {
    Fixture fixture;
    fixture.run(owl::Response::sse(from_chunks({"a\nb\nc"})).send(&fixture.req));
    fixture.drain();
    EXPECT_EQ(fixture.capture.body, "data: a\ndata: b\ndata: c\n\n");
}

TEST(Response, SseFramesEmptyPayload) {
    Fixture fixture;
    fixture.run(owl::Response::sse(from_chunks({""})).send(&fixture.req));
    fixture.drain();
    EXPECT_EQ(fixture.capture.body, "data: \n\n");
}

TEST(Response, SseLeavesContentLengthUnset) {
    Fixture fixture;
    fixture.run(owl::Response::sse(from_chunks({"hi"})).send(&fixture.req));
    fixture.drain();
    EXPECT_EQ(fixture.req.res.content_length, SIZE_MAX);
}

TEST(Response, DefaultTokenNeverCancels) {
    Fixture fixture;
    fixture.run(owl::Response::stream(from_chunks({"a", "b"}), "text/plain").send(&fixture.req));
    fixture.drain();
    EXPECT_EQ(fixture.capture.body, "ab");
    EXPECT_TRUE(fixture.capture.final_state);
}

TEST(Response, ClientDisconnectSendsNoFinalChunk) {
    Fixture fixture;
    fixture.run(owl::Response::stream(from_chunks({"a", "b"}), "text/plain").send(&fixture.req));
    EXPECT_EQ(fixture.capture.body, "a");
    const auto sends_before = fixture.capture.sends;
    fixture.stop();
    EXPECT_EQ(fixture.capture.sends, sends_before) << "client is gone; nothing more may be written";
}

TEST(Response, DiscardedResponseLeavesNoHeaders) {
    Fixture fixture;
    {
        auto discarded = owl::Response::ok("x");
        discarded.header("header-a", "A");
    }
    fixture.run(owl::Response::ok("hi").header("header-b", "B").send(&fixture.req));
    EXPECT_EQ(fixture.count("header-a"), 0u);
    EXPECT_EQ(fixture.count("header-b"), 1u);
}

TEST(Response, BranchesBuildIndependentResponses) {
    Fixture fixture;
    const auto pick = [](const bool condition) {
        if (condition) {
            return owl::Response::ok("hi").header("header-a", "A");
        }
        return owl::Response::ok("hi").header("header-b", "B");
    };

    fixture.run(pick(false).send(&fixture.req));
    EXPECT_EQ(fixture.count("header-a"), 0u);
    EXPECT_EQ(fixture.count("header-b"), 1u);
}

TEST(Response, UnsentResponseTouchesNothingOnTheRequest) {
    Fixture fixture;
    {
        auto unsent = owl::Response::json("{}", 500);
        unsent.header("x-ghost", "1").set_cookie({.name = "c", .value = "1"});
    }
    EXPECT_EQ(fixture.req.res.headers.size, 0u);
    EXPECT_EQ(fixture.req.res.status, 0);
}

TEST(Response, FromDoesNotAllocateFromPool) {
    Fixture fixture;
    EXPECT_EQ(fixture.req.pool.chunks, nullptr);
    {
        auto unused = owl::Response::ok("x");
    }
    EXPECT_EQ(fixture.req.pool.chunks, nullptr);
    EXPECT_EQ(fixture.req.pool.directs, nullptr);
}

TEST(Response, UnsentResponseDoesNotAllocateFromPool) {
    Fixture fixture;
    EXPECT_EQ(fixture.req.pool.chunks, nullptr);
    {
        auto unsent = owl::Response::json("{}", 500);
        unsent.header("x-ghost", "1").set_cookie({.name = "c", .value = "1"});
    }
    EXPECT_EQ(fixture.req.pool.chunks, nullptr);
    EXPECT_EQ(fixture.req.pool.directs, nullptr);
}
