#pragma once

// Status and body are fixed at construction so a Response is the value
// middleware threads through next() -- headers are the only thing added
// on the way out. Factories name the usual shapes; a free constructor
// would leave Content-Type unset.

#include <concepts>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <h2o.h>
#include <nlohmann/json.hpp>

#include <coro/async_generator.h>
#include <coro/task.h>
#include <coro/algo/fmap.h>

#include "cookie.h"
#include "detail/finish.h"
#include "policy.h"
#include "owl/ws/detail/upgrade.h"

namespace owl {
    using StreamBody = coro::async_generator<std::string>;
    using Body = std::variant<std::monostate, std::string, StreamBody, detail::WsUpgradePtr>;

    class Response final {
    public:
        [[nodiscard]] static Response ok(const std::string_view text, const int status = 200) {
            Response response{status, std::string{text}};
            response.set_token_header(H2O_TOKEN_CONTENT_TYPE, "text/plain; charset=utf-8");
            return response;
        }

        // string_view wins over nlohmann::json for const char*, so
        // json("{\"a\":1}") is raw JSON, not a dumped JSON string.
        [[nodiscard]] static Response json(const std::string_view body, const int status = 200) {
            Response response{status, std::string{body}};
            response.set_token_header(H2O_TOKEN_CONTENT_TYPE, "application/json");
            return response;
        }

        template <typename T> requires (!std::convertible_to<T, std::string_view>)
        [[nodiscard]] static Response json(const T& value, const int status = 200) {
            return json(std::string_view{nlohmann::json(value).dump()}, status);
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
            Response response{status, std::move(body) | coro::fmap(detail::frame_sse)};
            response.add_header("cache-control", "no-cache");
            response.set_token_header(H2O_TOKEN_CONTENT_TYPE, "text/event-stream");
            return response;
        }

        [[nodiscard]] int status() const noexcept {
            return status_;
        }

        [[nodiscard]] bool is_upgrade() const noexcept {
            return std::holds_alternative<detail::WsUpgradePtr>(body_);
        }

        // Status 101 is for middleware to read; nothing sends it. The driver
        // stages the headers and the engine writes the handshake.
        [[nodiscard]] static Response websocket(detail::WsUpgradePtr upgrade) {
            return Response{101, std::move(upgrade)};
        }

        // The driver's half of an upgrade. Headers and cookies middleware added
        // go into req->res, where h2o_http1_upgrade flattens them into the 101;
        // dropping them would silently lose a session cookie set on the way out.
        [[nodiscard]] detail::WsUpgradePtr stage_upgrade(this Response self, h2o_req_t* req) {
            self.flush_headers(req);
            return std::get<detail::WsUpgradePtr>(std::move(self.body_));
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
            if (self.is_upgrade()) {
                co_await Response::error(500).send(req);
                co_return;
            }

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
                const auto lowered = detail::dup(&req->pool, name);
                h2o_strtolower(lowered.base, lowered.len);
                const auto original = detail::dup(&req->pool, name);
                const auto copy = detail::dup(&req->pool, value);
                h2o_add_header_by_str(
                    &req->pool, &req->res.headers,
                    lowered.base, lowered.len,
                    1, original.base,
                    copy.base, copy.len
                );
            }
            for (const auto& cookie : cookies_) {
                const auto copy = detail::dup(&req->pool, cookie);
                h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_SET_COOKIE, nullptr, copy.base, copy.len);
            }
        }

        Body body_{};
        const int status_{200};
        const bool has_body_{false};

        std::vector<std::string> cookies_{};
        std::unordered_map<std::string, std::string> headers_;
    };
}
