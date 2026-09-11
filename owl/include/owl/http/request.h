#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stop_token>
#include <string_view>
#include <utility>

#include <h2o.h>

#include "owl/core/method.h"
#include "owl/util/pool_map.h"
#include "owl/util/util.h"

namespace owl {
    struct PathParam final {
        std::string_view name;
        std::string_view value;
    };

    inline constexpr std::size_t max_path_params = 8;

    class Request final {
    public:
        [[nodiscard]] static Request* make(h2o_req_t* const req) {
            const auto method_view = std::string_view{req->method.base, req->method.len};
            void* const memory = h2o_mem_alloc_shared(&req->pool, sizeof(Request), &dispose_request);
            return new(memory) Request{req, method_from_string(method_view), util::parse_query_req(req)};
        }

        [[nodiscard]] Method method() const noexcept {
            return method_;
        }

        [[nodiscard]] const h2o_req_t* raw() const noexcept {
            return req_;
        }

        [[nodiscard]] h2o_req_t* raw() noexcept {
            return req_;
        }

        [[nodiscard]] std::string_view path() const noexcept {
            return std::string_view{req_->path_normalized.base, req_->path_normalized.len};
        }

        [[nodiscard]] std::optional<std::string_view> header(const std::string_view name) const {
            if (!req_ || !req_->headers.entries) return std::nullopt;
            const auto entries = std::span<const h2o_header_t>{req_->headers.entries, req_->headers.size};

            const auto entry = std::ranges::find_if(entries, [name](const h2o_header_t& candidate) {
                return util::eq_ci(std::string_view{candidate.name->base, candidate.name->len}, name);
            });

            if (entry == entries.end()) return std::nullopt;
            return std::string_view{entry->value.base, entry->value.len};
        }

        [[nodiscard]] std::optional<std::string_view> query(const std::string_view name) const {
            if (const auto it = queries_.find(name); it != queries_.end()) {
                return it->second;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::string_view body() const noexcept {
            if (!req_ || req_->entity.base == nullptr) return {};
            return {req_->entity.base, req_->entity.len};
        }

        [[nodiscard]] std::optional<std::string_view> param(const std::string_view key) const noexcept {
            for (std::size_t i = 0; i < param_count_; ++i) {
                if (params_[i].name == key) return params_[i].value;
            }
            return std::nullopt;
        }

        // Matched route template, not the request path. Empty on miss.
        // Bounded (the registered pattern) so metrics can label without
        // cardinality bombs from /users/{id}.
        [[nodiscard]] std::string_view route_pattern() const noexcept {
            return route_pattern_;
        }

        void set_route_pattern(const std::string_view pattern) noexcept {
            route_pattern_ = pattern;
        }

        void add_param(const std::string_view name, const std::string_view value) noexcept {
            if (param_count_ < max_path_params) params_[param_count_++] = {.name = name, .value = value};
        }

        void clear_params() noexcept {
            param_count_ = 0;
        }

        [[nodiscard]] std::size_t param_count() const noexcept {
            return param_count_;
        }

        [[nodiscard]] std::stop_source stop_source() const noexcept {
            return stop_source_;
        }

        ~Request() {
            stop_source_.request_stop();
        }

    private:
        Request(
            h2o_req_t* const req,
            const Method method,
            pool_map queries
        ) : req_(req),
            method_(method),
            queries_(std::move(queries)) {
        }

        static void dispose_request(void* const memory) {
            std::destroy_at(static_cast<Request*>(memory));
        }

        h2o_req_t* const req_;
        const Method method_;
        const pool_map queries_;
        std::stop_source stop_source_{};
        std::array<PathParam, max_path_params> params_{};
        std::size_t param_count_{0};
        std::string_view route_pattern_{};
    };
}

template <>
struct std::formatter<owl::Request> : std::formatter<std::string_view> {
    template <typename Context>
    auto format(const owl::Request& req, Context& ctx) const {
        return std::formatter<std::string_view>::format(std::format("{} {}", req.method(), req.path()), ctx);
    }
};
