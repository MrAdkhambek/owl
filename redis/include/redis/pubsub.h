#pragma once

// subscribe: the messages of one SUBSCRIBE, as an async_generator. It asks
// the client for a connection of its own -- a subscribed connection answers
// nothing else, so the multiplexed command connection cannot be it -- and
// reads pushes forever: "message" pushes are yielded,
// everything else (the subscribe confirmations) is skipped. Failure is
// thrown, because that is how async_generator reports a body failure: at
// the consumer's next().
//
// Ending it. With no pull pending, destroy the generator: the frame dies
// at the co_yield it is parked on, the connection with it, and the server
// drops the subscription on EOF; there is no unsubscribe call. With a pull
// pending, destroying is not an option -- the frame is parked inside a
// reactor read that may never return, and the reactor holds a pointer into
// it -- so the consumer passes a std::stop_token and requests a stop. The
// stop_callback below cancels the wait: the parked read resumes with a
// cancelled status, the body sees the stop request and returns instead of
// throwing, and the pending next() completes with nullopt. Nothing is
// closed to achieve it, which is what makes the connection's fate the
// generator's own business. The stop reaches a pull that is still opening
// the connection too, since it is handed to the open; and one requested
// before the first pull opens nothing at all. The callback is declared
// after the connection so it dies first, and its destructor waits for a
// callback running on another thread.
//
// Where the stop may be requested from is the reactor's rule, not this
// one's: native_reactor takes a cancel from any thread, owl's loop_reactor
// only from its own loop -- which is where a handler's abort already is.
//
// A consumer that stops pulling but keeps the generator alive leaves the
// socket to fill until the server's client-output-buffer limit closes it.

#include <chrono>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <coro/async_generator.h>

#include "redis/client.h"
#include "redis/connection.h"
#include "redis/error.h"
#include "redis/reply.h"

namespace redis {
    struct message final {
        std::string channel;
        std::string payload;
    };

    [[nodiscard]] inline coro::async_generator<message>
    subscribe(const client& c, std::vector<std::string> channels, std::stop_token stop = {}) {
        // A stop that is already in has nothing to wait for, and a
        // connection opened for it would be a round trip for a subscription
        // nobody reads.
        if (stop.stop_requested()) co_return;

        // The client opens it, so a subscriber connection is made the same
        // way a command connection is; the generator holds nothing of the
        // client afterwards and may outlive the expression that named it.
        auto opened = co_await c.open_connection(stop);
        if (!opened) {
            if (stop.stop_requested()) co_return;
            throw std::move(opened.error());
        }
        const std::unique_ptr<connection> conn = std::move(*opened);
        if (stop.stop_requested()) co_return;

        const std::stop_callback wake{stop, [c = conn.get()] {
            c->cancel_wait();
        }};

        if (const detail::command_args argv{"SUBSCRIBE", channels}; !conn->append(argv.argv())) {
            throw error{error_kind::connection, "could not queue SUBSCRIBE"};
        }

        if (auto flushed = co_await conn->flush(std::chrono::milliseconds{-1}); !flushed) {
            if (stop.stop_requested()) co_return;
            throw std::move(flushed.error());
        }

        while (!stop.stop_requested()) {
            auto result = co_await conn->read(std::chrono::milliseconds{-1});
            if (!result) {
                if (stop.stop_requested()) co_return;
                throw std::move(result.error());
            }
            if (result->type() != reply_type::push || result->size() != 3) continue;
            if ((*result)[0].as<std::string_view>() != "message") continue;
            co_yield message{.channel = (*result)[1].as<std::string>(), .payload = (*result)[2].as<std::string>()};
        }
    }
}
