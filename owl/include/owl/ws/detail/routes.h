#pragma once

#include <memory>
#include <tuple>
#include <utility>

#include <coro/task.h>

#include "owl/ws/controller.h"
#include "owl/ws/detail/upgrade.h"
#include "owl/ws/socket.h"

namespace owl::detail {
    // One connection's call into the controller: the *shared* instance,
    // plus the extractors belonging to this connection alone.
    //
    // The shared_ptr is passed into the connection's frame, not just held
    // here: this object dies with the Response, and a frame that only
    // borrowed the instance would depend on the Router outliving every
    // connection. One refcount per connection buys that independence.
    //
    // Args is spelled out rather than deduced: deduction would turn a
    // `const T&` extractor into a copy of the Context member it binds.
    template <typename C, typename... Args>
    struct WsController final {
        std::shared_ptr<C> instance;

        template <typename... Arg>
        [[nodiscard]] coro::task<void> operator()(ws::Socket sock, Arg&&... arg) const {
            return ws::detail::controller_loop<C, Args...>(sock, instance, std::forward<Arg>(arg)...);
        }
    };

    // A route's start -- its coroutine, or a WsController -- and the tuple
    // extract_all returned, kept as it is. A value element owns its
    // extraction; a `const T&` element binds a Context member (a driver,
    // the loop), which outlives every connection on its worker. Views into
    // the request never reach this: ws_checks refuses them. Applying the
    // tuple as an rvalue hands a value element over as T&& and a reference
    // element as the reference it is, so no slot mapping is needed.
    template <typename Start, typename... Args>
    struct WsRoute final : WsUpgrade {
        Start start;
        std::tuple<Args...> args;

        WsRoute(Start fn, std::tuple<Args...> extracted)
            : start(std::move(fn)),
              args(std::move(extracted)) {
        }

        // The task is lazy, so the values move into its frame here --
        // before this object, owned by the Response, is gone.
        [[nodiscard]] coro::task<void> make(ws::detail::Session* session) override {
            return std::apply([this, session]<typename... Arg>(Arg&&... arg) {
                return start(ws::Socket{session->handle}, std::forward<Arg>(arg)...);
            }, std::move(args));
        }
    };
}
