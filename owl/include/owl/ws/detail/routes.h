#pragma once

#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include <coro/task.h>

#include "owl/ws/controller.h"
#include "owl/ws/detail/upgrade.h"
#include "owl/ws/socket.h"

namespace owl::detail {
    // A const& binds a Context member (a driver, the loop), which outlives
    // the connection, so a pointer to it is all the frame needs. A value is
    // owned outright. Views into the request never reach this: ws_checks
    // refuses them.
    template <typename Arg>
    using ws_slot = std::conditional_t<std::is_reference_v<Arg>,
                                       const std::remove_cvref_t<Arg>*,
                                       std::remove_cvref_t<Arg>>;

    template <typename Arg>
    [[nodiscard]] ws_slot<Arg> to_ws_slot(auto&& extracted) {
        if constexpr (std::is_reference_v<Arg>) return &extracted;
        else return std::forward<decltype(extracted)>(extracted);
    }

    template <typename Arg>
    [[nodiscard]] decltype(auto) from_ws_slot(ws_slot<Arg>& slot) {
        if constexpr (std::is_reference_v<Arg>) return *slot;
        else return std::move(slot);
    }

    template <typename... Args>
    [[nodiscard]] std::tuple<ws_slot<Args>...> to_ws_slots(std::tuple<Args...>&& extracted) {
        return [&]<std::size_t... I>(std::index_sequence<I...>) {
            return std::tuple<ws_slot<Args>...>{to_ws_slot<Args>(std::get<I>(std::move(extracted)))...};
        }(std::index_sequence_for<Args...>{});
    }

    template <typename... Args>
    struct WsRoute final : WsUpgrade {
        coro::task<void> (*handler)(ws::Socket, Args...);
        std::tuple<ws_slot<Args>...> slots;

        WsRoute(coro::task<void> (*fn)(ws::Socket, Args...), std::tuple<ws_slot<Args>...> extracted)
            : handler(fn),
              slots(std::move(extracted)) {
        }

        // The task is lazy, so the values are moved into its frame here --
        // before this object, owned by the Response, is gone.
        [[nodiscard]] coro::task<void> make(ws::detail::Session* session) override {
            return std::apply([this, session](auto&... slot) {
                return handler(ws::Socket{session}, from_ws_slot<Args>(slot)...);
            }, slots);
        }
    };

    // One connection's call into the controller: the *shared* instance,
    // plus the extractors belonging to this connection alone.
    //
    // Both halves are here for a reason. The instance is a shared_ptr
    // captured once at registration -- one controller serves every
    // connection, and holding it by shared_ptr is what lets the caller
    // keep a handle and reach it from outside the route. The extracted
    // values cannot join it there: they differ per connection, so a
    // controller that held them would overwrite one client's with the
    // next's. Hence one of these per request, built where the extraction
    // happens, exactly as WsRoute is. The instance outlives the connection
    // because the registered handler's capture holds it for the router's
    // lifetime.
    template <typename C, typename... Args>
    struct WsController final : WsUpgrade {
        std::shared_ptr<C> instance;
        std::tuple<ws_slot<Args>...> slots;

        WsController(
            std::shared_ptr<C> c,
            std::tuple<ws_slot<Args>...> extracted
        ) : instance(std::move(c)),
            slots(std::move(extracted)) {
        }

        [[nodiscard]] coro::task<void> make(ws::detail::Session* session) override {
            return std::apply([this, session](auto&... slot) {
                return ws::detail::controller_loop<C>(ws::Socket{session}, *instance, from_ws_slot<Args>(slot)...);
            }, slots);
        }
    };
}
