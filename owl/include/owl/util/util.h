#pragma once

#include <cctype>
#include <cstddef>
#include <ranges>
#include <string_view>
#include <utility>

#include <h2o.h>

#include "pool_map.h"

namespace owl::util {
    // ASCII case-insensitive character equality
    [[nodiscard]] inline bool
    eq_char_ci(const char x, const char y) noexcept {
        const auto ux = static_cast<unsigned char>(x);
        const auto uy = static_cast<unsigned char>(y);
        return ux == uy || std::tolower(ux) == std::tolower(uy);
    }

    // ASCII case-insensitive string equality
    [[nodiscard]] inline bool
    eq_ci(std::string_view a, std::string_view b) noexcept {
        return std::ranges::equal(a, b, eq_char_ci);
    }

    [[nodiscard]] constexpr bool is_word_char(const char c) noexcept {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    }

    [[nodiscard]] inline bool is_json_content_type(std::string_view ct) noexcept {
        while (!ct.empty() && (ct.front() == ' ' || ct.front() == '\t')) ct.remove_prefix(1);
        constexpr std::string_view json = "application/json";
        if (ct.size() < json.size() || !eq_ci(ct.substr(0, json.size()), json)) return false;
        if (ct.size() == json.size()) return true;
        const char next = ct[json.size()];
        return next == ';' || next == ' ' || next == '\t';
    }

    inline auto parse_query_req(h2o_req_t* const req) -> pool_map {
        if (req->path.base != nullptr && req->query_at != SIZE_MAX && req->query_at < req->path.len) {
            return std::string_view(req->path.base, req->path.len).substr(req->query_at + 1)
                | std::views::split('&')
                | std::views::transform([](auto&& rng) {
                    return std::string_view{rng};
                })
                | std::views::filter([](const std::string_view view) {
                    return !view.empty();
                })
                | std::views::transform([](std::string_view view) {
                    const auto eq_pos = view.find('=');
                    return eq_pos == std::string_view::npos
                               ? std::pair<std::string_view, std::string_view>{view, {}}
                               : std::pair{view.substr(0, eq_pos), view.substr(eq_pos + 1)};
                })
                | std::ranges::to<pool_map>();
        }

        return pool_map{};
    }
}
