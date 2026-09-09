#include <gtest/gtest.h>

#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <coro/io/reactor.h>
#include <coro/io/reactor_ref.h>
#include <coro/run/sync_wait.h>
#include <coro/task.h>

#include <redis/config.h>
#include <redis/connection.h>
#include <redis/error.h>
#include <redis/reply.h>

#include "support/fake_server.h"
#include "support/fixture.h"
#include "support/resp.h"

using namespace std::chrono_literals;
using namespace redis_test;

namespace {

    using opened = std::expected<std::unique_ptr<redis::connection>, redis::error>;

    opened open(fixture& fx, const redis::config cfg) {
        return coro::sync_wait(redis::connection::open(cfg, fx.io));
    }

    std::string_view what(const opened& c) {
        return c ? "" : c.error().what();
    }
}

TEST(Connection, OpensWithHelloAndClosesOnDestruction) {
    fake_server srv{{script{expect{hello}, send_bytes{hello_ok}, expect_eof{}}}};
    fixture fx;
    {
        const auto c = open(fx, {.port = srv.port()});
        EXPECT_TRUE(c.has_value()) << what(c);
        if (c) {
            EXPECT_TRUE((*c)->ok());
            EXPECT_GE((*c)->fd(), 0);
        }
    }
    srv.join();
}

TEST(Connection, AuthAndSelectFollowHello) {
    fake_server srv{{script{
        expect{cmd({"HELLO", "3", "AUTH", "default", "pw"})}, send_bytes{hello_ok},
        expect{cmd({"SELECT", "2"})}, send_bytes{"+OK\r\n"},
        expect_eof{}}}};
    fixture fx;
    {
        const auto c = open(fx, {.port = srv.port(), .password = "pw", .db = 2});
        EXPECT_TRUE(c.has_value()) << what(c);
    }
    srv.join();
}

TEST(Connection, AuthUsesTheUsernameWhenGiven) {
    fake_server srv{{script{expect{cmd({"HELLO", "3", "AUTH", "alice", "pw"})}, send_bytes{hello_ok}, expect_eof{}}}};
    fixture fx;
    {
        const auto c = open(fx, {.port = srv.port(), .username = "alice", .password = "pw"});
        EXPECT_TRUE(c.has_value()) << what(c);
    }
    srv.join();
}

TEST(Connection, HelloErrorIsConnect) {
    fake_server srv{{script{
        expect{cmd({"HELLO", "3", "AUTH", "default", "bad"})},
        send_bytes{"-WRONGPASS invalid username-password pair or user is disabled.\r\n"},
        expect_eof{}}}};
    fixture fx;
    {
        const auto c = open(fx, {.port = srv.port(), .password = "bad"});
        EXPECT_FALSE(c.has_value());
        if (!c) {
            EXPECT_EQ(c.error().kind(), redis::error_kind::connect);
            EXPECT_NE(std::string_view{c.error().what()}.find("WRONGPASS"), std::string_view::npos);
        }
    }
    srv.join();
}

TEST(Connection, OldServerNamesResp3) {
    fake_server srv{{script{expect{hello}, send_bytes{"-ERR unknown command 'HELLO'\r\n"}, expect_eof{}}}};
    fixture fx;
    {
        const auto c = open(fx, {.port = srv.port()});
        EXPECT_FALSE(c.has_value());
        if (!c) {
            EXPECT_EQ(c.error().kind(), redis::error_kind::connect);
            EXPECT_STREQ(c.error().what(), "server does not speak RESP3 (HELLO): Redis 6 or newer is required");
        }
    }
    srv.join();
}

TEST(Connection, SelectErrorIsConnect) {
    fake_server srv{{script{expect{hello}, send_bytes{hello_ok}, expect{cmd({"SELECT", "99"})}, send_bytes{"-ERR DB index is out of range\r\n"}, expect_eof{}}}};
    fixture fx;
    {
        const auto c = open(fx, {.port = srv.port(), .db = 99});
        EXPECT_FALSE(c.has_value());
        if (!c) EXPECT_EQ(c.error().kind(), redis::error_kind::connect);
    }
    srv.join();
}

TEST(Connection, RefusedPortIsConnect) {
    fixture fx;
    const auto c = open(fx, {.port = 1, .connect_timeout = 2000ms});
    EXPECT_FALSE(c.has_value());
    if (!c) EXPECT_EQ(c.error().kind(), redis::error_kind::connect);
}

TEST(Connection, SilentServerIsTimeout) {
    fake_server srv{{script{expect{hello}, sleep_for{300ms}, close_now{}}}};
    fixture fx;
    {
        const auto c = open(fx, {.port = srv.port(), .connect_timeout = 50ms});
        EXPECT_FALSE(c.has_value());
        if (!c) EXPECT_EQ(c.error().kind(), redis::error_kind::timeout);
    }
    srv.join();
}

TEST(Connection, AppendFlushRead) {
    fake_server srv{{script{expect{hello}, send_bytes{hello_ok}, expect{cmd({"GET", "k"})}, send_bytes{"$1\r\nv\r\n"}, expect_eof{}}}};
    fixture fx;
    {
        const auto c = open(fx, {.port = srv.port()});
        EXPECT_TRUE(c.has_value()) << what(c);
        if (c) {
            coro::sync_wait([&]() -> coro::task<> {
                const std::vector<std::string_view> argv{"GET", "k"};
                EXPECT_TRUE((*c)->append(argv));
                const auto f = co_await (*c)->flush(-1ms);
                EXPECT_TRUE(f.has_value());
                const auto r = co_await (*c)->read(-1ms);
                EXPECT_TRUE(r.has_value());
                if (r) EXPECT_EQ(r->as<std::string>(), "v");
                EXPECT_TRUE((*c)->ok());
            }());
        }
    }
    srv.join();
}

TEST(Connection, PushComesBackAsIsAndErrorAsCommand) {
    fake_server srv{{script{
        expect{hello}, send_bytes{hello_ok},
        send_bytes{">2\r\n$4\r\npong\r\n$0\r\n\r\n-ERR boom\r\n"},
        expect_eof{}}}};
    fixture fx;
    {
        const auto c = open(fx, {.port = srv.port()});
        EXPECT_TRUE(c.has_value()) << what(c);
        if (c) {
            coro::sync_wait([&]() -> coro::task<> {
                const auto push = co_await (*c)->read(-1ms);
                EXPECT_TRUE(push.has_value());
                if (push) {
                    EXPECT_EQ(push->type(), redis::reply_type::push);
                    EXPECT_EQ(push->size(), 2u);
                }
                const auto err = co_await (*c)->read(-1ms);
                EXPECT_FALSE(err.has_value());
                if (!err) {
                    EXPECT_EQ(err.error().kind(), redis::error_kind::command);
                    EXPECT_EQ(err.error().prefix(), "ERR");
                    EXPECT_STREQ(err.error().what(), "ERR boom");
                }
                EXPECT_TRUE((*c)->ok());
            }());
        }
    }
    srv.join();
}

TEST(Connection, EofIsConnection) {
    fake_server srv{{script{expect{hello}, send_bytes{hello_ok}, close_now{}}}};
    fixture fx;
    {
        const auto c = open(fx, {.port = srv.port()});
        EXPECT_TRUE(c.has_value()) << what(c);
        if (c) {
            coro::sync_wait([&]() -> coro::task<> {
                const auto r = co_await (*c)->read(-1ms);
                EXPECT_FALSE(r.has_value());
                if (!r) EXPECT_EQ(r.error().kind(), redis::error_kind::connection);
                EXPECT_FALSE((*c)->ok());
            }());
        }
    }
    srv.join();
}

TEST(Connection, ReadTimeoutFinishesTheConnection) {
    fake_server srv{{script{expect{hello}, send_bytes{hello_ok}, expect{cmd({"PING"})}, sleep_for{300ms}, close_now{}}}};
    fixture fx;
    {
        const auto c = open(fx, {.port = srv.port()});
        EXPECT_TRUE(c.has_value()) << what(c);
        if (c) {
            coro::sync_wait([&]() -> coro::task<> {
                const std::vector<std::string_view> argv{"PING"};
                EXPECT_TRUE((*c)->append(argv));
                const auto f = co_await (*c)->flush(-1ms);
                EXPECT_TRUE(f.has_value());
                const auto r = co_await (*c)->read(50ms);
                EXPECT_FALSE(r.has_value());
                if (!r) EXPECT_EQ(r.error().kind(), redis::error_kind::timeout);
                EXPECT_FALSE((*c)->ok());
                EXPECT_EQ((*c)->fd(), -1);
                EXPECT_FALSE((*c)->append(argv));
            }());
        }
    }
    srv.join();
}
