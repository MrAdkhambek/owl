#include "rest/auth.h"

#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace rest {
    namespace {
        std::string hex(const unsigned char* const bytes, const std::size_t n) {
            static constexpr char digits[] = "0123456789abcdef";
            std::string out;
            out.reserve(n * 2);
            for (std::size_t i = 0; i < n; ++i) {
                out += digits[bytes[i] >> 4];
                out += digits[bytes[i] & 0xF];
            }
            return out;
        }

        std::string random_hex(const std::size_t bytes) {
            unsigned char buf[32];
            if (bytes > sizeof buf || RAND_bytes(buf, static_cast<int>(bytes)) != 1) {
                throw std::runtime_error("RAND_bytes failed");
            }
            return hex(buf, bytes);
        }

        std::string derive(const std::string_view password, const std::string_view salt) {
            unsigned char key[32];
            PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()),
                              reinterpret_cast<const unsigned char*>(salt.data()), static_cast<int>(salt.size()),
                              100'000, EVP_sha256(), sizeof key, key);
            return hex(key, sizeof key);
        }

        // Constant time, so a wrong password costs the same however wrong.
        bool same(const std::string_view a, const std::string_view b) {
            return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
        }
    }

    coro::task<std::string> hash_password(const std::string password) {
        const auto salt = random_hex(16);
        co_return salt + ":" + derive(password, salt);
    }

    coro::task<bool> verify_password(const std::string password, const std::string stored) {
        const auto colon = stored.find(':');
        if (colon == std::string::npos) co_return false;
        const std::string_view view{stored};
        co_return same(derive(password, view.substr(0, colon)), view.substr(colon + 1));
    }

    std::string new_token() {
        return random_hex(32);
    }

    coro::task<> store_session(const Cache& cache, const std::string_view token, const std::int64_t user_id) {
        co_await cache.set("session:" + std::string{token}, std::to_string(user_id), session_ttl);
    }

    coro::task<bool> login_throttled(const Cache& cache, const std::string_view username) {
        const std::string key = "login:" + std::string{username};
        const auto attempts = co_await cache.incr(key);
        // The first attempt starts the minute; the counter dies with it.
        if (attempts == 1) (void)co_await cache.expire(key, std::chrono::minutes{1});
        co_return attempts > login_attempts_per_minute;
    }

    coro::task<std::optional<std::int64_t>> user_of(const Cache& cache, const Bearer bearer) {
        const auto r = co_await cache.command("GET", "session:" + std::string{bearer.token});
        co_return r.as<std::optional<std::int64_t>>();
    }
}
