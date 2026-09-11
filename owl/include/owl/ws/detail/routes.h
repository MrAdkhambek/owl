#pragma once

#include <tuple>
#include <type_traits>
#include <utility>

#include <coro/task.h>

#include "owl/ws/detail/upgrade.h"
#include "owl/ws/socket.h"

namespace owl::detail {
    // A const& binds a Context member (a driver, the loop), which outlives
    // the connection, so a pointer to it is all the frame needs. A value is
    // owned outright. Views into the request never reach this: ws_checks
    // refuses them.
    template <typename Arg>
    using WsSlot = std::conditional_t<std::is_reference_v<Arg>,
                                      const std::remove_cvref_t<Arg>*,
                                      std::remove_cvref_t<Arg>>;

    template <typename Arg>
    [[nodiscard]] WsSlot<Arg> to_ws_slot(auto&& extracted) {
        if constexpr (std::is_reference_v<Arg>) return &extracted;
        else return std::move(extracted);
    }

    template <typename Arg>
    [[nodiscard]] decltype(auto) from_ws_slot(WsSlot<Arg>& slot) {
        if constexpr (std::is_reference_v<Arg>) return *slot;
        else return std::move(slot);
    }

    template <typename... Args>
    [[nodiscard]] std::tuple<WsSlot<Args>...> to_ws_slots(std::tuple<Args...>&& extracted) {
        return [&]<std::size_t... I>(std::index_sequence<I...>) {
            return std::tuple<WsSlot<Args>...>{to_ws_slot<Args>(std::get<I>(std::move(extracted)))...};
        }(std::index_sequence_for<Args...>{});
    }

    template <typename... Args>
    struct WsRoute final : WsUpgrade {
        coro::task<void> (*handler)(ws::Socket, Args...);
        std::tuple<WsSlot<Args>...> slots;

        WsRoute(coro::task<void> (*fn)(ws::Socket, Args...), std::tuple<WsSlot<Args>...> extracted)
            : handler(fn), slots(std::move(extracted)) {
        }

        // The task is lazy, so the values are moved into its frame here --
        // before this object, owned by the Response, is gone.
        [[nodiscard]] coro::task<void> make(ws::detail::Session* session) override {
            return std::apply([this, session](auto&... slot) {
                return handler(ws::Socket{session}, from_ws_slot<Args>(slot)...);
            }, slots);
        }
    };
}
