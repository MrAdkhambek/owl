#pragma once

// Next returns the handler's Response so middleware can add headers on the
// way out or skip next() and kick. A framed Response<F> could not be that
// type: one chain wraps mixed routes.

#include <cstddef>
#include <functional>
#include <vector>

#include <coro/task.h>

#include "owl/core/state.h"
#include "owl/http/request.h"
#include "owl/http/response.h"

namespace owl {
    template <typename S>
    class Next;

    template <typename S>
    using Middleware = std::function<coro::task<Response>(const Request&, const Context<S>&, Next<S>)>;

    template <typename S>
    using MiddlewareChain = std::vector<Middleware<S>>;

    template <typename S>
    using Handler = std::function<coro::task<Response>(const Request&, const Context<S>&)>;

    template <typename S>
    class Terminal final {
    public:
        explicit Terminal(const Handler<S>* handler) noexcept : handler_(handler) {
        }

        [[nodiscard]] coro::task<Response> operator()(const Request& req, const Context<S>& ctx) const {
            return (*handler_)(req, ctx);
        }

    private:
        const Handler<S>* handler_ = nullptr;
    };

    template <typename S>
    class Next final {
    public:
        Next(
            const MiddlewareChain<S>* const* chains,
            const std::size_t chain_count,
            Terminal<S>* terminal,
            const Context<S>* ctx
        ) noexcept
            : chains_(chains),
              chain_count_(chain_count),
              terminal_(terminal),
              ctx_(ctx) {
        }

        [[nodiscard]] coro::task<Response> operator()(const Request& req) const {
            std::size_t chain = chain_index_;
            std::size_t item = item_index_;
            while (chain < chain_count_ && item >= chains_[chain]->size()) {
                ++chain;
                item = 0;
            }
            if (chain >= chain_count_) return (*terminal_)(req, *ctx_);
            const Middleware<S>& current = (*chains_[chain])[item];
            return current(req, *ctx_, Next{chains_, chain_count_, chain, item + 1, terminal_, ctx_});
        }

    private:
        Next(
            const MiddlewareChain<S>* const* chains,
            const std::size_t chain_count,
            const std::size_t chain_index,
            const std::size_t item_index,
            Terminal<S>* terminal,
            const Context<S>* ctx
        ) noexcept
            : chains_(chains),
              chain_count_(chain_count),
              chain_index_(chain_index),
              item_index_(item_index),
              terminal_(terminal),
              ctx_(ctx) {
        }

        const MiddlewareChain<S>* const* chains_;
        std::size_t chain_count_;
        std::size_t chain_index_ = 0;
        std::size_t item_index_ = 0;
        Terminal<S>* terminal_;
        const Context<S>* ctx_;
    };
}
