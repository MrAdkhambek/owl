#pragma once

// What a connection is opened with. One struct serves the client and every
// subscriber connection it spawns, so a subscription is authenticated and
// on the same database as the commands. Budgets follow the reactor's
// convention: a negative duration is no deadline.

#include <chrono>
#include <cstdint>
#include <string>

namespace redis {
    struct config final {
        std::string host = "127.0.0.1";
        std::uint16_t port = 6379;
        // Empty username with a password authenticates the default user.
        std::string username;
        std::string password;
        unsigned db = 0;
        // Covers the TCP connect and the whole handshake.
        std::chrono::milliseconds connect_timeout{5000};
        // Per command, from the moment it is queued to its reply.
        std::chrono::milliseconds command_timeout{-1};
    };
}
