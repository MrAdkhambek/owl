#pragma once

// R is the handler's return type -- Response or coro::task<Response>.
// Carrying it in the type is what lets mount() wrap a synchronous handler
// instead of forcing every handler to be a coroutine.

#include <type_traits>

#include <coro/task.h>

#include "owl/core/method.h"
#include "owl/http/response.h"

namespace owl::detail {
    template <Method M, typename R, typename... Args>
    struct Endpoint {
        static constexpr Method method = M;

        R (*handler)(Args...);
    };

    template <class R>
    inline constexpr bool is_handler_return_v =
        std::is_same_v<R, Response> || std::is_same_v<R, coro::task<Response>>;
}
