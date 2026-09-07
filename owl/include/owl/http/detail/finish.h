#pragma once

#include <cstddef>
#include <coroutine>
#include <string>
#include <string_view>
#include <utility>

#include <h2o.h>

#include <coro/async_generator.h>
#include <coro/task.h>

#include "owl/http/policy.h"

namespace owl::detail {
    [[nodiscard]] static std::string frame_sse(const std::string_view payload) {
        if (payload.empty()) return "data: \n\n";

        const auto n_lines = 1 + static_cast<std::size_t>(std::ranges::count(payload, '\n'));

        std::string framed;
        framed.reserve(payload.size() + n_lines * 6 + 1); // "data: " per line + final '\n'

        for (auto&& part : payload | std::views::split('\n')) {
            std::format_to(std::back_inserter(framed), "data: {}\n", std::string_view(part));
        }
        framed.push_back('\n');
        return framed;
    }

    struct stream_channel final {
    private:
        static void on_proceed(h2o_generator_t* const self, h2o_req_t*) noexcept {
            reinterpret_cast<stream_channel*>(self)->wake();
        }

        static void on_stop(h2o_generator_t* const self, h2o_req_t*) noexcept {
            auto* const channel = reinterpret_cast<stream_channel*>(self);
            channel->client_gone = true;
            channel->wake();
        }

    public:
        h2o_generator_t super{};
        std::coroutine_handle<> parked{};
        bool client_gone{false};

        [[nodiscard]] static stream_channel* from(h2o_req_t* req) {
            const auto channel = static_cast<stream_channel*>(
                h2o_mem_alloc_pool_aligned(&req->pool, alignof(stream_channel), sizeof(stream_channel)));
            channel->super = {
                .proceed = &on_proceed,
                .stop = &on_stop,
            };
            channel->parked = {};
            channel->client_gone = false;
            return channel;
        }

        void wake() noexcept {
            if (const auto continuation = std::exchange(this->parked, {})) {
                continuation.resume();
            }
        }
    };

    struct await_proceed final {
        stream_channel* channel;

        [[nodiscard]] bool halted() const noexcept {
            return channel->client_gone;
        }

        [[nodiscard]] bool await_ready() const noexcept {
            return halted();
        }

        bool await_suspend(const std::coroutine_handle<> parked) const noexcept {
            if (halted()) return false;
            channel->parked = parked;
            return true;
        }

        [[nodiscard]] bool await_resume() const noexcept {
            return !halted();
        }
    };

    [[nodiscard]] inline h2o_iovec_t dup(h2o_mem_pool_t* pool, const std::string_view text) {
        if (text.empty()) return h2o_iovec_init("", 0);
        return h2o_strdup(pool, text.data(), text.size());
    }

    inline void write_status(h2o_req_t* const req, const int status) noexcept {
        req->res.status = status;
        req->res.reason = policy::reason_phrase(status);
    }

    inline void add_token_header(h2o_req_t* const req, const h2o_token_t* const token, const std::string_view value) {
        const auto copy = value.empty() ? h2o_iovec_init("", 0) : h2o_strdup(&req->pool, value.data(), value.size());
        h2o_add_header(&req->pool, &req->res.headers, token, nullptr, copy.base, copy.len);
    }

    inline coro::task<void> write_stream(h2o_req_t* const req, coro::async_generator<std::string> gen) {
        auto* const channel = stream_channel::from(req);
        h2o_start_response(req, &channel->super);
        const await_proceed proceed{.channel = channel};

        while (!proceed.halted()) {
            auto chunk = co_await gen.next();
            if (!chunk.has_value()) break;
            if (chunk->empty()) continue;
            if (proceed.halted()) break;

            auto buf = h2o_iovec_init(chunk->data(), chunk->size());
            h2o_send(req, &buf, 1, H2O_SEND_STATE_IN_PROGRESS);

            if (!co_await proceed) break;
        }

        if (!channel->client_gone) {
            h2o_send(req, nullptr, 0, H2O_SEND_STATE_FINAL);
        }
    }

    inline void send_error_floor(h2o_req_t* req, const int status) noexcept {
        h2o_send_error_generic(req, status, policy::reason_phrase(status), policy::reason_phrase(status), 0);
    }

    inline void send_inline(h2o_req_t* const req, const int status, const char* const body, const std::size_t len) {
        write_status(req, status);
        h2o_send_inline(req, body, len);
    }

    inline void send_not_found(h2o_req_t* const req) noexcept {
        send_inline(req, 404, H2O_STRLIT("not found"));
    }

    inline void send_not_allowed(h2o_req_t* const req, const std::string_view allow) {
        const auto [base, len] = h2o_strdup(&req->pool, allow.data(), allow.size());
        h2o_add_header_by_str(&req->pool, &req->res.headers, H2O_STRLIT("allow"), 0, nullptr, base, len);
        send_inline(req, 405, H2O_STRLIT("method not allowed"));
    }
}
