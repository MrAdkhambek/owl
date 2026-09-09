#pragma once

// Passwords, tokens, and the Bearer extractor: axum's auth module and its
// extractor in one place.

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include <coro/task.h>
#include <owl/owl.h>

#include "rest/app.h"

namespace rest {
    // PBKDF2-HMAC-SHA256 with a fresh salt, stored as "salt:key" in hex.
    [[nodiscard]] std::string hash_password(std::string_view password);
    [[nodiscard]] bool verify_password(std::string_view password, std::string_view stored);

    // 32 random bytes, in hex.
    [[nodiscard]] std::string new_token();

    // The token from the Authorization header. A missing or malformed
    // header kicks 401 before the handler runs.
    struct Bearer {
        std::string_view token;
    };

    // The user a token stands for, or nothing.
    [[nodiscard]] coro::task<std::optional<std::int64_t>> user_of(const Db& db, Bearer bearer);
}

template <>
struct owl::FromContext<rest::Bearer> {
    template <typename S>
    std::expected<rest::Bearer, owl::KickToken> operator()(const owl::Context<S>&, const owl::Request& req) const {
        const auto auth = req.header("authorization");
        if (!auth || !auth->starts_with("Bearer ")) return std::unexpected(owl::KickToken{"missing bearer token", 401});
        return rest::Bearer{auth->substr(7)};
    }
};
