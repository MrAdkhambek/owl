#include <gtest/gtest.h>

#include <chrono>
#include <expected>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <coro/algo/when_all.h>
#include <coro/io/reactor.h>
#include <coro/io/reactor_ref.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <redis/client.h>
#include <redis/config.h>
#include <redis/error.h>
#include <redis/reply.h>

#include "support/fake_server.h"
#include "support/resp.h"

using namespace std::chrono_literals;
using redis_test::close_now;
using redis_test::cmd;
using redis_test::expect;
using redis_test::expect_eof;
using redis_test::fake_server;
using redis_test::script;
using redis_test::send_bytes;
using redis_test::sleep_for;

namespace {
    const std::string hello = cmd({"HELLO", "3"});
    const std::string hello_ok = "%1\r\n$5\r\nproto\r\n:3\r\n";
    const std::string ping = cmd({"PING"});
    const std::string pong = "+PONG\r\n";

    struct fixture final {
        coro::native_reactor reactor;
        coro::reactor_ref io{reactor};

        // Every resumption lands on the reactor thread; a body that starts
        // concurrent commands from cold hops there first so a completion
        // cannot race the main thread. sleep parks and resumes there.
        coro::task<> hop() {
            (void)co_await reactor.sleep(1ms);
        }
    };

    using result = std::expected<redis::reply, redis::error>;

    coro::task<result> run(const redis::client& c, const std::vector<std::string_view> argv) {
        co_return co_await c.try_command(argv);
    }

    std::string status(const result& r) {
        if (r) return r->as<std::string>();
        return "<" + std::string{redis::to_string(r.error().kind())} + ": " + r.error().what() + ">";
    }
}

TEST(Client, FirstCommandOpensLazilyAndCloseDropsTheConnection) {
    fake_server srv{{script{expect{hello}, send_bytes{hello_ok}, expect{ping}, send_bytes{pong}, expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    EXPECT_FALSE(c.connected());
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await run(c, {"PING"});
        EXPECT_EQ(status(r), "PONG");
        EXPECT_TRUE(c.connected());
        EXPECT_EQ(c.in_flight(), 0u);
    }());
    c.close();
    EXPECT_FALSE(c.connected());
    srv.join();
}

TEST(Client, PipelinesConcurrentCommandsAndAnswersInOrder) {
    fake_server srv{{script{
        expect{hello}, send_bytes{hello_ok}, expect{ping}, send_bytes{pong},
        expect{cmd({"GET", "a"}) + cmd({"GET", "b"})}, send_bytes{"$1\r\n1\r\n$1\r\n2\r\n"},
        expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        (void)co_await run(c, {"PING"});
        const auto [a, b] = co_await coro::when_all(run(c, {"GET", "a"}), run(c, {"GET", "b"}));
        EXPECT_EQ(status(a), "1");
        EXPECT_EQ(status(b), "2");
        EXPECT_EQ(c.in_flight(), 0u);
    }());
    c.close();
    srv.join();
}

// Three or more pipelined replies buffered at once: the head passes the
// baton from inside a drain loop, which is where a waiter sitting in two
// queues on one link loses the rest of the FIFO.
TEST(Client, FourPipelinedCommandsWithRepliesBufferedTogether) {
    fake_server srv{{script{
        expect{hello}, send_bytes{hello_ok}, expect{ping}, send_bytes{pong},
        expect{cmd({"GET", "a"}) + cmd({"GET", "b"}) + cmd({"GET", "c"}) + cmd({"GET", "d"})},
        send_bytes{"$1\r\n1\r\n$1\r\n2\r\n$1\r\n3\r\n$1\r\n4\r\n"},
        expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        (void)co_await run(c, {"PING"});
        const auto [a, b, cc, d] = co_await coro::when_all(run(c, {"GET", "a"}), run(c, {"GET", "b"}), run(c, {"GET", "c"}), run(c, {"GET", "d"}));
        EXPECT_EQ(status(a), "1");
        EXPECT_EQ(status(b), "2");
        EXPECT_EQ(status(cc), "3");
        EXPECT_EQ(status(d), "4");
        EXPECT_EQ(c.in_flight(), 0u);
    }());
    c.close();
    srv.join();
}

TEST(Client, CallersArrivingWhileOpeningAllComplete) {
    fake_server srv{{script{expect{hello}, send_bytes{hello_ok}, expect{ping + ping}, send_bytes{pong + pong}, expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        co_await fx.hop();
        const auto [a, b] = co_await coro::when_all(run(c, {"PING"}), run(c, {"PING"}));
        EXPECT_EQ(status(a), "PONG");
        EXPECT_EQ(status(b), "PONG");
    }());
    c.close();
    srv.join();
}

TEST(Client, CommandErrorKeepsTheConnection) {
    fake_server srv{{script{
        expect{hello}, send_bytes{hello_ok},
        expect{cmd({"INCR", "k"})}, send_bytes{"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"},
        expect{ping}, send_bytes{pong},
        expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await run(c, {"INCR", "k"});
        EXPECT_FALSE(r.has_value());
        if (!r) {
            EXPECT_EQ(r.error().kind(), redis::error_kind::command);
            EXPECT_EQ(r.error().prefix(), "WRONGTYPE");
        }
        EXPECT_TRUE(c.connected());
        EXPECT_EQ(status(co_await run(c, {"PING"})), "PONG");
    }());
    c.close();
    srv.join();
}

TEST(Client, PushOnTheCommandConnectionIsDropped) {
    fake_server srv{{script{
        expect{hello}, send_bytes{hello_ok},
        expect{ping}, send_bytes{">2\r\n$10\r\ninvalidate\r\n$1\r\nk\r\n" + pong},
        expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        EXPECT_EQ(status(co_await run(c, {"PING"})), "PONG");
    }());
    c.close();
    srv.join();
}

TEST(Client, EofFailsEveryInFlightCommandAndTheNextReconnects) {
    fake_server srv{{
        script{expect{hello}, send_bytes{hello_ok}, expect{ping}, send_bytes{pong}, expect{cmd({"GET", "a"}) + cmd({"GET", "b"})}, close_now{}},
        script{expect{hello}, send_bytes{hello_ok}, expect{ping}, send_bytes{pong}, expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        (void)co_await run(c, {"PING"});
        const auto [a, b] = co_await coro::when_all(run(c, {"GET", "a"}), run(c, {"GET", "b"}));
        EXPECT_FALSE(a.has_value());
        if (!a) EXPECT_EQ(a.error().kind(), redis::error_kind::connection);
        EXPECT_FALSE(b.has_value());
        if (!b) {
            EXPECT_EQ(b.error().kind(), redis::error_kind::connection);
            EXPECT_TRUE(std::string_view{b.error().what()}.starts_with("connection dropped"));
        }
        EXPECT_FALSE(c.connected());
        EXPECT_EQ(c.in_flight(), 0u);
        EXPECT_EQ(status(co_await run(c, {"PING"})), "PONG");
        EXPECT_TRUE(c.connected());
    }());
    c.close();
    srv.join();
}

TEST(Client, TimeoutFailsTheHeadAndDropsTheRest) {
    fake_server srv{{script{
        expect{hello}, send_bytes{hello_ok}, expect{ping}, send_bytes{pong},
        expect{cmd({"GET", "a"}) + cmd({"GET", "b"})}, sleep_for{400ms}, close_now{}}}};
    fixture fx;
    redis::client c{{.port = srv.port(), .command_timeout = 100ms}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        (void)co_await run(c, {"PING"});
        const auto [a, b] = co_await coro::when_all(run(c, {"GET", "a"}), run(c, {"GET", "b"}));
        EXPECT_FALSE(a.has_value());
        if (!a) EXPECT_EQ(a.error().kind(), redis::error_kind::timeout);
        EXPECT_FALSE(b.has_value());
        if (!b) EXPECT_EQ(b.error().kind(), redis::error_kind::connection);
        EXPECT_FALSE(c.connected());
    }());
    srv.join();
}

TEST(Client, CloseRefusesNewCommandsAndLetsInFlightFinish) {
    fake_server srv{{script{
        expect{hello}, send_bytes{hello_ok},
        expect{cmd({"GET", "a"})}, sleep_for{200ms}, send_bytes{"$1\r\n1\r\n"},
        expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        co_await fx.hop();
        auto closer = [&]() -> coro::task<> {
            (void)co_await fx.reactor.sleep(50ms);
            EXPECT_EQ(c.in_flight(), 1u);
            c.close();
            const auto refused = co_await run(c, {"PING"});
            EXPECT_FALSE(refused.has_value());
            if (!refused) EXPECT_EQ(refused.error().kind(), redis::error_kind::closed);
        };
        const auto results = co_await coro::when_all(run(c, {"GET", "a"}), closer());
        EXPECT_EQ(status(std::get<0>(results)), "1");
        EXPECT_FALSE(c.connected());
    }());
    srv.join();
}

TEST(Client, CloseFailsCallersParkedOnConnect) {
    fake_server srv{{script{expect{hello}, sleep_for{150ms}, send_bytes{hello_ok}, expect_eof{}}}};
    fixture fx;
    redis::client c{{.port = srv.port()}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        co_await fx.hop();
        auto closer = [&]() -> coro::task<> {
            (void)co_await fx.reactor.sleep(30ms);
            c.close();
        };
        const auto results = co_await coro::when_all(run(c, {"PING"}), run(c, {"PING"}), closer());
        for (const result* r : {&std::get<0>(results), &std::get<1>(results)}) {
            EXPECT_FALSE(r->has_value());
            if (!r->has_value()) EXPECT_EQ(r->error().kind(), redis::error_kind::closed);
        }
        EXPECT_FALSE(c.connected());
    }());
    srv.join();
}

TEST(Client, RefusedConnectionIsConnect) {
    fixture fx;
    redis::client c{{.port = 1, .connect_timeout = 2000ms}, fx.io};
    coro::sync_wait([&]() -> coro::task<> {
        const auto r = co_await run(c, {"PING"});
        EXPECT_FALSE(r.has_value());
        if (!r) EXPECT_EQ(r.error().kind(), redis::error_kind::connect);
        EXPECT_FALSE(c.connected());
        EXPECT_EQ(c.in_flight(), 0u);
    }());
}
