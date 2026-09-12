#pragma once

#include <memory>
#include <tuple>
#include <utility>

#include <coro/task.h>

#include "owl/ws/controller.h"
#include "owl/ws/detail/upgrade.h"
#include "owl/ws/socket.h"

namespace owl::detail {
    // The tuple extract_all returned, kept as it is. A value element owns
    // its extraction; a `const T&` element binds a Context member (a driver,
    // the loop), which outlives every connection on its worker. Views into
    // the request never reach this: ws_checks refuses them. Applying the
    // tuple as an rvalue hands a value element over as T&& and a reference
    // element as the reference it is, so no slot mapping is needed.
    template <typename... Args>
    struct WsRoute final : WsUpgrade {
        coro::task<void> (*handler)(ws::Socket, Args...);
        std::tuple<Args...> args;

        WsRoute(coro::task<void> (*fn)(ws::Socket, Args...), std::tuple<Args...> extracted)
            : handler(fn),
              args(std::move(extracted)) {
        }

        // The task is lazy, so the values move into its frame here --
        // before this object, owned by the Response, is gone.
        [[nodiscard]] coro::task<void> make(ws::detail::Session* session) override {
            return std::apply([this, session](auto&&... arg) {
                return handler(ws::Socket{session->handle}, std::forward<decltype(arg)>(arg)...);
            }, std::move(args));
        }
    };

    // One connection's call into the controller: the *shared* instance,
    // plus the extractors belonging to this connection alone.
    //
    // The shared_ptr is passed into the connection's frame, not just held
    // here: this object dies with the Response, and a frame that only
    // borrowed the instance would depend on the Router outliving every
    // connection. One refcount per connection buys that independence.
    template <typename C, typename... Args>
    struct WsController final : WsUpgrade {
        std::shared_ptr<C> instance;
        std::tuple<Args...> args;

        WsController(
            std::shared_ptr<C> c,
            std::tuple<Args...> extracted
        ) : instance(std::move(c)),
            args(std::move(extracted)) {
        }

        // Args is spelled out rather than deduced: deduction would turn a
        // `const T&` extractor into a copy of the Context member it binds.
        [[nodiscard]] coro::task<void> make(ws::detail::Session* session) override {
            return std::apply([this, session](auto&&... arg) {
                return ws::detail::controller_loop<C, Args...>(ws::Socket{session->handle}, instance, std::forward<decltype(arg)>(arg)...);
            }, std::move(args));
        }
    };
}
