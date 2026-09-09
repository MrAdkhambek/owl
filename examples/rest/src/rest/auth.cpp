#include "rest/auth.h"

#include <cstddef>
#include <stdexcept>

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

    std::string hash_password(const std::string_view password) {
        const auto salt = random_hex(16);
        return salt + ":" + derive(password, salt);
    }

    bool verify_password(const std::string_view password, const std::string_view stored) {
        const auto colon = stored.find(':');
        if (colon == std::string_view::npos) return false;
        return same(derive(password, stored.substr(0, colon)), stored.substr(colon + 1));
    }

    std::string new_token() {
        return random_hex(32);
    }

    coro::task<std::optional<std::int64_t>> user_of(const Db& db, const Bearer bearer) {
        const auto r = co_await sql::query<"SELECT user_id FROM sessions WHERE token = $1">(db, bearer.token);
        if (r.rows() == 0) co_return std::nullopt;
        co_return r[0][0].as<std::int64_t>();
    }
}
