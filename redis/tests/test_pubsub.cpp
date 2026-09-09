#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <stop_token>
#include <string>
#include <tuple>
#include <vector>

#include <coro/algo/when_all.h>
#include <coro/io/reactor.h>
#include <coro/io/reactor_ref.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <redis/client.h>
#include <redis/error.h>
#include <redis/pubsub.h>

#include "support/fake_server.h"
#include "support/fixture.h"
#include "support/resp.h"

using namespace std::chrono_literals;
using namespace redis_test;

namespace {
    const std::string subscribe_news = cmd({"SUBSCRIBE", "news"});
    const std::string subscribed = ">3\r\n$9\r\nsubscribe\r\n$4\r\nnews\r\n:1\r\n";

    std::string message(const std::string& channel, const std::string& payload) {
        return ">3\r\n$7\r\nmessage\r\n$" + std::to_string(channel.size()) + "\r\n" + channel + "\r\n$"
            + std::to_string(payload.size()) + "\r\n" + payload + "\r\n";
    }
}

TEST(Pubsub, YieldsMessagesSkipsConfirmationsAndClosesOnDestruction) {
    fake_server srv{{script{
        expect{hello}, send_bytes{hello_ok},
        expect{subscribe_news}, send_bytes{subscribed},
        send_bytes{message("news", "hi") + message("news", "bye")},
        expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        auto gen = redis::subscribe(c, {"news"});
        const auto first = co_await gen.next();
        EXPECT_TRUE(first.has_value());
        if (first) {
            EXPECT_EQ(first->channel, "news");
            EXPECT_EQ(first->payload, "hi");
        }
        const auto second = co_await gen.next();
        EXPECT_TRUE(second.has_value());
        if (second) EXPECT_EQ(second->payload, "bye");
    }());
    EXPECT_FALSE(c.connected());
    srv.join();
}

TEST(Pubsub, TwoChannelsInOneSubscribe) {
    fake_server srv{{script{
        expect{hello}, send_bytes{hello_ok},
        expect{cmd({"SUBSCRIBE", "a", "b"})}, send_bytes{message("b", "x")},
        expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        auto gen = redis::subscribe(c, {"a", "b"});
        const auto m = co_await gen.next();
        EXPECT_TRUE(m.has_value());
        if (m) EXPECT_EQ(m->channel, "b");
    }());
    srv.join();
}

TEST(Pubsub, ServerCloseSurfacesAsConnectionAtNext) {
    fake_server srv{{script{expect{hello}, send_bytes{hello_ok}, expect{subscribe_news}, send_bytes{message("news", "hi")}, close_now{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        auto gen = redis::subscribe(c, {"news"});
        (void)co_await gen.next();
        bool thrown = false;
        try {
            (void)co_await gen.next();
        } catch (const redis::error& e) {
            thrown = true;
            EXPECT_EQ(e.kind(), redis::error_kind::connection);
        }
        EXPECT_TRUE(thrown);
    }());
    srv.join();
}

TEST(Pubsub, OpenFailureSurfacesAtFirstNext) {
    fixture fx;
    redis::client c{{.port = 1, .connect_timeout = 2000ms}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        auto gen = redis::subscribe(c, {"news"});
        bool thrown = false;
        try {
            (void)co_await gen.next();
        } catch (const redis::error& e) {
            thrown = true;
            EXPECT_EQ(e.kind(), redis::error_kind::connect);
        }
        EXPECT_TRUE(thrown);
    }());
}

TEST(Pubsub, RequestStopCompletesAPendingPullWithNullopt) {
    fake_server srv{{script{expect{hello}, send_bytes{hello_ok}, expect{subscribe_news}, send_bytes{subscribed}, expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    std::stop_source stop;
    coro::sync_wait([&]() -> coro::task<> {
        co_await fx.hop();
        auto gen = redis::subscribe(c, {"news"}, stop.get_token());
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
    srv.join();
}

TEST(Pubsub, StopRequestedBeforeTheFirstPullEndsWithoutSubscribing) {
    fake_server srv{{script{expect{hello}, send_bytes{hello_ok}, expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    std::stop_source stop;
    stop.request_stop();
    coro::sync_wait([&]() -> coro::task<> {
        auto gen = redis::subscribe(c, {"news"}, stop.get_token());
        const auto m = co_await gen.next();
        EXPECT_FALSE(m.has_value());
    }());
    srv.join();
}
