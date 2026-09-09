#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

#include <coro/algo/when_all.h>
#include <coro/io/reactor.h>
#include <coro/io/reactor_ref.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <redis/redis.h>

using namespace std::chrono_literals;

namespace {
    // OWL_TEST_REDIS=host:port, or just host for port 6379.
    std::optional<redis::config> live_config() {
        const char* const env = std::getenv("OWL_TEST_REDIS");
        if (env == nullptr) return std::nullopt;
        const std::string_view spec{env};
        redis::config cfg;
        if (const auto colon = spec.rfind(':'); colon != std::string_view::npos) {
            cfg.host = std::string{spec.substr(0, colon)};
            cfg.port = static_cast<std::uint16_t>(std::stoi(std::string{spec.substr(colon + 1)}));
        } else {
            cfg.host = std::string{spec};
        }
        return cfg;
    }

    struct fixture final {
        coro::native_reactor reactor;
        coro::reactor_ref io{reactor};
        redis::client c;

        explicit fixture(redis::config cfg) : c(std::move(cfg), io) {
        }

        ~fixture() {
            c.close();
        }

        std::string key(const std::string_view name) const {
            return "owl_test:" + std::to_string(::getpid()) + ":" + std::string{name};
        }
    };
}

#define OWL_REQUIRE_REDIS()                                                                                                   \
    const auto live = live_config();                                                                                          \
    if (!live) GTEST_SKIP() << "OWL_TEST_REDIS is not set"

TEST(Live, SetGetRoundTrip) {
    OWL_REQUIRE_REDIS();
    fixture fx{*live};
    coro::sync_wait([&]() -> coro::task<> {
        const auto k = fx.key("kv");
        co_await fx.c.set(k, "value");
        EXPECT_EQ(co_await fx.c.get(k), std::optional<std::string>{"value"});
        EXPECT_EQ(co_await fx.c.del(k), 1);
        EXPECT_EQ(co_await fx.c.get(k), std::nullopt);
    }());
}

TEST(Live, IncrExpireTtl) {
    OWL_REQUIRE_REDIS();
    fixture fx{*live};
    coro::sync_wait([&]() -> coro::task<> {
        const auto k = fx.key("counter");
        (void)co_await fx.c.del(k);
        EXPECT_EQ(co_await fx.c.incr(k), 1);
        EXPECT_EQ(co_await fx.c.incr(k), 2);
        EXPECT_TRUE(co_await fx.c.expire(k, 60s));
        const auto ttl = co_await fx.c.command("TTL", k);
        EXPECT_GT(ttl.as<int>(), 0);
        (void)co_await fx.c.del(k);
    }());
}

TEST(Live, EvalRateLimitScript) {
    OWL_REQUIRE_REDIS();
    fixture fx{*live};
    coro::sync_wait([&]() -> coro::task<> {
        const auto k = fx.key("rate");
        (void)co_await fx.c.del(k);
        const std::vector<std::string> keys{k};
        const std::vector<std::string_view> args{"60"};
        const std::string_view script =
            "local n = redis.call('INCR', KEYS[1]) "
            "if n == 1 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end "
            "return n";
        EXPECT_EQ((co_await fx.c.eval(script, keys, args)).as<int>(), 1);
        EXPECT_EQ((co_await fx.c.eval(script, keys, args)).as<int>(), 2);
        (void)co_await fx.c.del(k);
    }());
}

TEST(Live, SixteenConcurrentIncrsCount) {
    OWL_REQUIRE_REDIS();
    fixture fx{*live};
    coro::sync_wait([&]() -> coro::task<> {
        const auto k = fx.key("concurrent");
        (void)co_await fx.c.del(k);
        std::vector<coro::task<std::expected<std::int64_t, redis::error>>> incrs;
        for (int i = 0; i < 16; ++i) incrs.push_back(fx.c.try_incr(k));
        const auto results = co_await coro::when_all(std::move(incrs));
        for (const auto& r : results) EXPECT_TRUE(r.has_value());
        EXPECT_EQ(co_await fx.c.get(k), std::optional<std::string>{"16"});
        EXPECT_EQ(fx.c.in_flight(), 0u);
        (void)co_await fx.c.del(k);
    }());
}

TEST(Live, SubscribeReceivesAPublish) {
    OWL_REQUIRE_REDIS();
    fixture fx{*live};
    coro::sync_wait([&]() -> coro::task<> {
        const auto channel = fx.key("channel");
        (void)co_await fx.c.try_command("PING");
        bool got = false;
        auto consumer = [&]() -> coro::task<> {
            auto gen = redis::subscribe(fx.c, {channel});
            const auto m = co_await gen.next();
            EXPECT_TRUE(m.has_value());
            if (m) {
                EXPECT_EQ(m->channel, channel);
                EXPECT_EQ(m->payload, "hello");
            }
            got = true;
        };
        // The subscribe confirmation is not surfaced, so publish until the
        // consumer has its message.
        auto producer = [&]() -> coro::task<> {
            while (!got) {
                (void)co_await fx.c.try_publish(channel, "hello");
                (void)co_await fx.reactor.sleep(20ms);
            }
        };
        (void)co_await coro::when_all(consumer(), producer());
    }());
}

TEST(Live, StopEndsAPendingSubscription) {
    OWL_REQUIRE_REDIS();
    fixture fx{*live};
    std::stop_source stop;
    coro::sync_wait([&]() -> coro::task<> {
        (void)co_await fx.c.try_command("PING");
        auto gen = redis::subscribe(fx.c, {fx.key("silent")}, stop.get_token());
        auto puller = [&]() -> coro::task<> {
            const auto m = co_await gen.next();
            EXPECT_FALSE(m.has_value());
        };
        auto stopper = [&]() -> coro::task<> {
            (void)co_await fx.reactor.sleep(100ms);
            stop.request_stop();
        };
        (void)co_await coro::when_all(puller(), stopper());
    }());
}

TEST(Live, WrongTypeCarriesItsPrefix) {
    OWL_REQUIRE_REDIS();
    fixture fx{*live};
    coro::sync_wait([&]() -> coro::task<> {
        const auto k = fx.key("hash");
        (void)co_await fx.c.command("HSET", k, "f", "v");
        const auto r = co_await fx.c.try_incr(k);
        EXPECT_FALSE(r.has_value());
        if (!r) {
            EXPECT_EQ(r.error().kind(), redis::error_kind::command);
            EXPECT_EQ(r.error().prefix(), "WRONGTYPE");
        }
        EXPECT_TRUE(fx.c.connected());
        (void)co_await fx.c.del(k);
    }());
}

// A wrong password alone is not enough: a default user configured with
// nopass accepts any password. A user that does not exist is refused always.
TEST(Live, UnknownUserIsConnect) {
    OWL_REQUIRE_REDIS();
    redis::config bad = *live;
    bad.username = "owl-no-such-user";
    bad.password = "definitely-not-the-password";
    fixture fx{bad};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await fx.c.try_command("PING");
        EXPECT_FALSE(r.has_value());
        if (!r) EXPECT_EQ(r.error().kind(), redis::error_kind::connect);
    }());
}
