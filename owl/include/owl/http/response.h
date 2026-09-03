#pragma once

// Status and body are fixed at construction so a Response is the value
// middleware threads through next() -- headers are the only thing added
// on the way out. Factories name the usual shapes; a free constructor
// would leave Content-Type unset.

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <h2o.h>

#include <coro/async_generator.h>
#include <coro/task.h>
#include <coro/algo/fmap.h>

#include "cookie.h"
#include "detail/finish.h"
#include "policy.h"

namespace owl {
    using StreamBody = coro::async_generator<std::string>;
    using Body = std::variant<std::monostate, std::string, StreamBody>;

    namespace detail {
        [[nodiscard]] inline std::string frame_sse(const std::string_view payload) {
            const auto n_lines = 1 + static_cast<std::size_t>(std::ranges::count(payload, '\n'));

            std::string framed;
            framed.reserve(payload.size() + n_lines * 6 + 1); // "data: " per line + final '\n'

            for (auto&& part : payload | std::views::split('\n')) {
                std::format_to(std::back_inserter(framed), "data: {}\n", std::string_view(part));
            }
            framed.push_back('\n');
            return framed;
        }
    }

    class Response final {
    public:
        [[nodiscard]] static Response ok(const std::string_view text, const int status = 200) {
            Response response{status, std::string{text}};
            response.set_token_header(H2O_TOKEN_CONTENT_TYPE, "text/plain; charset=utf-8");
            return response;
        }

        [[nodiscard]] static Response json(const nlohmann::json& json, const int status = 200) {
            Response response{status, json.dump()};
            response.set_token_header(H2O_TOKEN_CONTENT_TYPE, "application/json");
            return response;
        }

        [[nodiscard]] static Response body(
            const std::string_view body,
            const std::string_view content_type,
            const int status = 200
        ) {
            Response response{status, std::string{body}};
            response.set_token_header(H2O_TOKEN_CONTENT_TYPE, content_type);
            return response;
        }

        [[nodiscard]] static Response error(const int status) {
            return ok(policy::canned_reason(status), status);
        }

        [[nodiscard]] static Response no_content() {
            return Response{204, std::monostate{}};
        }

        [[nodiscard]] static Response redirect(const std::string_view location, const int status = 302) {
            Response response{status, std::string{}};
            response.set_token_header(H2O_TOKEN_LOCATION, location);
            return response;
        }

        [[nodiscard]] static Response stream(
            StreamBody body,
            const std::string_view content_type,
            const int status = 200
        ) {
            Response response{status, std::move(body)};
            response.set_token_header(H2O_TOKEN_CONTENT_TYPE, content_type);
            return response;
        }

        [[nodiscard]] static Response sse(StreamBody body, const int status = 200) {
            Response response{
                status,
                std::move(body) | coro::fmap([](const std::string& chunk) {
                    return owl::detail::frame_sse(chunk);
                }),
            };
            response.add_header("cache-control", "no-cache");
            response.set_token_header(H2O_TOKEN_CONTENT_TYPE, "text/event-stream");
            return response;
        }

        [[nodiscard]] int status() const noexcept {
            return status_;
        }

        template <typename Self>
        auto&& header(this Self&& self, const std::string_view name, const std::string_view value) {
            self.add_header(name, value);
            return std::forward<Self>(self);
        }

        template <typename Self>
        auto&& set_cookie(this Self&& self, const cookie& c) {
            self.add_token_header(H2O_TOKEN_SET_COOKIE, format_cookie(c));
            return std::forward<Self>(self);
        }

        coro::task<> send(this Response self, h2o_req_t* req) {
            detail::write_status(req, self.status_);
            self.flush_headers(req);

            if (auto* stream = std::get_if<StreamBody>(&self.body_)) {
                co_await detail::write_stream(req, std::move(*stream));
                co_return;
            }

            if (const auto* text = std::get_if<std::string>(&self.body_); text != nullptr && self.has_body_) {
                req->res.content_length = text->size();
                h2o_send_inline(req, text->data(), text->size());
                co_return;
            }

            h2o_send_inline(req, "", 0);
        }

    private:
        explicit Response(const int status, Body body) noexcept
            : status_(status),
              has_body_(std::holds_alternative<std::string>(body)),
              body_(std::move(body)) {
        }

        [[nodiscard]] h2o_iovec_t dup(h2o_mem_pool_t* pool, const std::string_view text) const {
            if (text.empty()) return h2o_iovec_init("", 0);
            return h2o_strdup(pool, text.data(), text.size());
        }

        void add_token_header(const h2o_token_t* const token, const std::string_view value) {
            if (token == H2O_TOKEN_SET_COOKIE) {
                cookies_.emplace_back(value);
                return;
            }
            headers_.insert_or_assign({token->buf.base, token->buf.len}, std::string{value});
        }

        void set_token_header(const h2o_token_t* const token, const std::string_view value) {
            headers_.insert_or_assign({token->buf.base, token->buf.len}, std::string{value});
        }

        void add_header(const std::string_view name, const std::string_view value) {
            headers_.insert_or_assign(std::string{name}, std::string{value});
        }

        void flush_headers(h2o_req_t* req) {
            for (const auto& [name, value] : headers_) {
                const auto lowered = dup(&req->pool, name);
                h2o_strtolower(lowered.base, lowered.len);
                const auto original = dup(&req->pool, name);
                const auto copy = dup(&req->pool, value);
                h2o_add_header_by_str(&req->pool, &req->res.headers, lowered.base, lowered.len, 1,
                                      original.base, copy.base, copy.len);
            }
            for (const auto& cookie : cookies_) {
                const auto copy = dup(&req->pool, cookie);
                h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_SET_COOKIE, nullptr, copy.base, copy.len);
            }
        }

        const int status_{200};
        const bool has_body_{false};

        std::vector<std::string> cookies_{};
        std::unordered_map<std::string, std::string> headers_;
        Body body_{};
    };
}
