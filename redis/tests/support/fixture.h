#pragma once

// What every fake-server suite needs: the bytes of a RESP3 handshake, and a
// reactor to drive one connection on. Here rather than copied into each
// suite because coro's own support header records what copying costs --
// four copies of a fixture drifted apart, and one of them quietly stopped
// testing what every assertion around it claimed.

#include <chrono>
#include <string>

#include <coro/io/reactor.h>
#include <coro/io/reactor_ref.h>
#include <coro/task.h>

#include "support/resp.h"

namespace redis_test {
    // What open() exchanges before it hands a connection out, and the
    // cheapest command to follow it with.
    inline const std::string hello = cmd({"HELLO", "3"});
    inline const std::string hello_ok = "%1\r\n$5\r\nproto\r\n:3\r\n";
    inline const std::string ping = cmd({"PING"});
    inline const std::string pong = "+PONG\r\n";

    struct fixture final {
        coro::native_reactor reactor;
        coro::reactor_ref io{reactor};

        // Every resumption lands on the reactor thread; a body that starts
        // concurrent work from cold hops there first, so a completion on
        // the reactor thread cannot race the main one. sleep parks and
        // resumes there.
        coro::task<> hop() {
            (void)co_await reactor.sleep(std::chrono::milliseconds{1});
        }
    };
}
