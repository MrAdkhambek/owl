#pragma once

// The two shapes Router::layer / Server::Builder::layer accept. A layer that
// wants state takes const Context<S>& rather than State<T>: the Context is
// already on the chain, so no second injection path is maintained.

#include <concepts>

#include <coro/task.h>

#include "owl/core/state.h"
#include "owl/http/request.h"
#include "owl/http/response.h"
#include "owl/routing/middleware.h"

namespace owl::detail {
    template <typename F, typename S>
    concept ContextLayer = requires(F f, const Request& r, const Context<S>& c, Next<S> n) {
        { f(r, c, n) } -> std::convertible_to<coro::task<Response>>;
    };

    template <typename F, typename S>
    concept PlainLayer = requires(F f, const Request& r, Next<S> n) {
        { f(r, n) } -> std::convertible_to<coro::task<Response>>;
    };
}
