#pragma once

#include <expected>
#include <string>
#include <utility>

namespace owl {
    class KickToken final {
    public:
        static constexpr auto default_status = 500;

        explicit KickToken(std::string message, const int status = default_status)
            : status_(status), message_(std::move(message)) {
        }

        [[nodiscard]] int status() const noexcept { return status_; }

        [[nodiscard]] const std::string& message() const & noexcept { return message_; }

        [[nodiscard]] std::string message() && noexcept { return std::move(message_); }

        [[nodiscard]] static std::unexpected<KickToken>
        not_found(std::string message = "not found") {
            return std::unexpected(KickToken(std::move(message), 404));
        }

        [[nodiscard]] static std::unexpected<KickToken>
        bad_request(std::string message = "bad request") {
            return std::unexpected(KickToken(std::move(message), 400));
        }

        [[nodiscard]] static std::unexpected<KickToken>
        unsupported_media_type(std::string message = "unsupported media type") {
            return std::unexpected(KickToken(std::move(message), 415));
        }

        [[nodiscard]] static std::unexpected<KickToken>
        unprocessable(std::string message = "unprocessable content") {
            return std::unexpected(KickToken(std::move(message), 422));
        }

        [[nodiscard]] static std::unexpected<KickToken>
        internal_error(std::string message = "internal error") {
            return std::unexpected(KickToken(std::move(message), default_status));
        }

    private:
        int status_ = default_status;
        std::string message_;
    };
}
