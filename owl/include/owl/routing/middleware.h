#pragma once

// Next returns the handler's Response so middleware can add headers on the
// way out or skip next() and kick. A framed Response<F> could not be that
// type: one chain wraps mixed routes.

#include <cstddef>
#include <functional>
#include <vector>

#include <coro/task.h>

#include "owl/http/request.h"
#include "owl/http/response.h"

namespace owl {
    class Next;

    using Middleware = std::function<coro::task<Response>(const Request&, Next)>;
    using MiddlewareChain = std::vector<Middleware>;
    using Handler = std::function<coro::task<Response>(const Request&)>;

    class Terminal final {
    public:
        explicit Terminal(const Handler* handler) noexcept : handler_(handler) {
        }

        [[nodiscard]] coro::task<Response> operator()(const Request& req) const {
            return (*handler_)(req);
        }

    private:
        const Handler* handler_ = nullptr;
    };

    class Next final {
    public:
        Next(
            const MiddlewareChain* const* chains,
            const std::size_t chain_count,
            Terminal* terminal
        ) noexcept
            : chains_(chains),
              chain_count_(chain_count),
              terminal_(terminal) {
        }

        [[nodiscard]] coro::task<Response> operator()(const Request& req) const {
            std::size_t chain = chain_index_;
            std::size_t item = item_index_;
            while (chain < chain_count_ && item >= chains_[chain]->size()) {
                ++chain;
                item = 0;
            }
            if (chain >= chain_count_) return (*terminal_)(req);
            const Middleware& current = (*chains_[chain])[item];
            return current(req, Next{chains_, chain_count_, chain, item + 1, terminal_});
        }

    private:
        Next(
            const MiddlewareChain* const* chains,
            const std::size_t chain_count,
            const std::size_t chain_index,
            const std::size_t item_index, Terminal* terminal
        ) noexcept
            : chains_(chains),
              chain_count_(chain_count),
              chain_index_(chain_index),
              item_index_(item_index),
              terminal_(terminal) {
        }

        const MiddlewareChain* const* chains_;
        std::size_t chain_count_;
        std::size_t chain_index_ = 0;
        std::size_t item_index_ = 0;
        Terminal* terminal_;
    };
}
