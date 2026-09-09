#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace owl {
    enum class same_site : std::uint8_t { unspecified, strict, lax, none };

    struct cookie final {
        std::string_view name{};
        std::string_view value{};
        std::string_view path{};
        std::string_view domain{};
        std::optional<std::chrono::seconds> max_age{};
        bool http_only{false};
        bool secure{false};
        // Qualified: a member named after its type is fine, but GCC refuses the
        // unqualified spelling ([basic.scope.class], -Wchanges-meaning).
        owl::same_site same_site{};
    };

    [[nodiscard]] inline std::string_view to_string(const same_site policy) noexcept {
        switch (policy) {
            case same_site::strict: return "Strict";
            case same_site::lax: return "Lax";
            case same_site::none: return "None";
            case same_site::unspecified: return {};
        }
        return {};
    }

    [[nodiscard]] inline std::string format_cookie(const cookie& c) {
        std::string out{c.name};
        out += '=';
        out += c.value;

        if (!c.path.empty()) {
            out += "; Path=";
            out += c.path;
        }
        if (!c.domain.empty()) {
            out += "; Domain=";
            out += c.domain;
        }
        if (c.max_age.has_value()) {
            out += std::format("; Max-Age={}", c.max_age->count());
        }
        if (c.http_only) out += "; HttpOnly";
        if (c.secure) out += "; Secure";
        if (const auto policy = to_string(c.same_site); !policy.empty()) {
            out += "; SameSite=";
            out += policy;
        }

        return out;
    }
}

template <>
struct std::formatter<owl::same_site> : std::formatter<std::string_view> {
    template <typename Context>
    auto format(const owl::same_site policy, Context& ctx) const {
        return std::formatter<std::string_view>::format(owl::to_string(policy), ctx);
    }
};

template <>
struct std::formatter<owl::cookie> : std::formatter<std::string_view> {
    template <typename Context>
    auto format(const owl::cookie& c, Context& ctx) const {
        return std::formatter<std::string_view>::format(owl::format_cookie(c), ctx);
    }
};
