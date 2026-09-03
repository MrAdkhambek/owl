#pragma once

#include <string>

namespace owl::policy {
    [[nodiscard]] inline const char* reason_phrase(const int status) noexcept {
        switch (status) {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 415: return "Unsupported Media Type";
        case 422: return "Unprocessable Content";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default: return "Unknown";
        }
    }

    [[nodiscard]] inline std::string canned_reason(const int status) {
        switch (status) {
        case 404: return "not found";
        case 405: return "method not allowed";
        case 501: return "not implemented";
        default: return "error";
        }
    }
}
