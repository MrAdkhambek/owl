#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
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

    // A loopback port that takes connections into its backlog and never
    // accepts or answers them: a client's connect completes and its HELLO
    // goes unanswered, so an open parks in the handshake. It can also say
    // whether anyone connected at all, which a scripted server cannot.
    class silent_listener final {
    public:
        silent_listener() : fd_(::socket(AF_INET, SOCK_STREAM, 0)) {
            EXPECT_GE(fd_, 0);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            EXPECT_EQ(::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr), 0);
            EXPECT_EQ(::listen(fd_, 8), 0);
            socklen_t len = sizeof addr;
            ::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
            port_ = ntohs(addr.sin_port);
        }

        ~silent_listener() {
            if (fd_ >= 0) ::close(fd_);
        }

        silent_listener(const silent_listener&) = delete;
        silent_listener& operator=(const silent_listener&) = delete;

        [[nodiscard]] std::uint16_t port() const noexcept {
            return port_;
        }

        [[nodiscard]] bool anyone_connected() const {
            pollfd p{.fd = fd_, .events = POLLIN, .revents = 0};
            return ::poll(&p, 1, 0) > 0;
        }

    private:
        int fd_ = -1;
        std::uint16_t port_ = 0;
    };
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

// The window a reactor cancel alone cannot cover: between one message and
// the next pull nothing is parked, so there is no wait to cancel and the
// stop has to be carried by the connection itself.
TEST(Pubsub, AStopBetweenPullsIsNotLost) {
    fake_server srv{{script{
        expect{hello}, send_bytes{hello_ok},
        expect{subscribe_news}, send_bytes{message("news", "hi")},
        expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    std::stop_source stop;
    coro::sync_wait([&]() -> coro::task<> {
        auto gen = redis::subscribe(c, {"news"}, stop.get_token());
        const auto first = co_await gen.next();
        EXPECT_TRUE(first.has_value());
        stop.request_stop();
        const auto second = co_await gen.next();
        EXPECT_FALSE(second.has_value());
    }());
    srv.join();
}

// A stop that is already in has nothing to wait for: the first pull ends at
// once, and no connection is opened for a subscription nobody will read.
TEST(Pubsub, StopRequestedBeforeTheFirstPullNeverConnects) {
    const silent_listener server;
    fixture fx;
    redis::client c{{.port = server.port(), .connect_timeout = 300ms}, fx.io};
    std::stop_source stop;
    stop.request_stop();
    coro::sync_wait([&]() -> coro::task<> {
        auto gen = redis::subscribe(c, {"news"}, stop.get_token());
        const auto m = co_await gen.next();
        EXPECT_FALSE(m.has_value());
    }());
    EXPECT_FALSE(server.anyone_connected());
}

// The server takes the connection and never answers HELLO, so the pull is
// parked inside the open when the stop arrives. It has to end there, with
// nullopt, rather than wait out connect_timeout and then throw.
TEST(Pubsub, RequestStopEndsAPullStillOpeningTheConnection) {
    const silent_listener server;
    fixture fx;
    redis::client c{{.port = server.port(), .connect_timeout = 3000ms}, fx.io};
    std::stop_source stop;
    const auto started = std::chrono::steady_clock::now();
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
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s) << "the stop waited out connect_timeout";
}
