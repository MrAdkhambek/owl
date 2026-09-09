#pragma once

// A scripted RESP peer: listens on an ephemeral loopback port on its own
// thread and, per accepted connection, follows a list of steps -- read
// exactly these bytes and check them, send these bytes, sleep, close, expect
// EOF. Deterministic bytes in and out is what the connection and client
// tests need; nothing here parses RESP. join() reports any step that never
// ran, which is how a test notices the client never sent what it should.
// SIGPIPE is ignored process-wide on construction: a scripted close makes
// the client's next write a broken pipe.

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace redis_test {
    struct expect final {
        std::string bytes;
    };

    struct send_bytes final {
        std::string bytes;
    };

    struct sleep_for final {
        std::chrono::milliseconds duration;
    };

    struct close_now final {
    };

    struct expect_eof final {
    };

    using step = std::variant<expect, send_bytes, sleep_for, close_now, expect_eof>;
    using script = std::vector<step>;

    class fake_server final {
    public:
        explicit fake_server(std::vector<script> connections) : scripts_(std::move(connections)) {
            std::signal(SIGPIPE, SIG_IGN);
            for (const auto& s : scripts_) total_ += s.size();

            listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (listener_ < 0) throw std::runtime_error("fake_server: socket");
            const int yes = 1;
            ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            if (::bind(listener_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
                throw std::runtime_error("fake_server: bind");
            }
            if (::listen(listener_, 8) != 0) throw std::runtime_error("fake_server: listen");
            socklen_t len = sizeof addr;
            ::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
            port_ = ntohs(addr.sin_port);

            thread_ = std::thread([this] {
                run();
            });
        }

        ~fake_server() {
            join();
        }

        fake_server(const fake_server&) = delete;
        fake_server& operator=(const fake_server&) = delete;

        [[nodiscard]] std::uint16_t port() const noexcept {
            return port_;
        }

        // Waits up to five seconds for every scripted step to run, stops the
        // thread, and reports steps that never ran. Call it after the client
        // side is torn down; the destructor calls it too.
        void join() {
            if (!thread_.joinable()) return;
            const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (!finished_.load() && std::chrono::steady_clock::now() < give_up) {
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            stop_.store(true);
            thread_.join();
            ::close(listener_);
            EXPECT_EQ(ran_.load(), total_) << "fake server: " << (total_ - ran_.load()) << " scripted step(s) never ran";
        }

    private:
        void run() {
            for (const script& s : scripts_) {
                const int fd = accept_or_stop();
                if (fd < 0) break;
                const timeval tv{.tv_sec = 5, .tv_usec = 0};
                ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
                const int yes = 1;
                ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
                bool open = true;
                for (const step& st : s) {
                    const bool ran = std::visit([&](const auto& x) {
                        return do_step(fd, open, x);
                    }, st);
                    if (ran) ++ran_;
                    if (!open) break;
                }
                if (open) ::close(fd);
            }
            finished_.store(true);
        }

        [[nodiscard]] int accept_or_stop() const {
            pollfd p{.fd = listener_, .events = POLLIN, .revents = 0};
            while (!stop_.load()) {
                if (::poll(&p, 1, 50) > 0) return ::accept(listener_, nullptr, nullptr);
            }
            return -1;
        }

        static bool do_step(const int fd, bool& open, const expect& e) {
            std::string got(e.bytes.size(), '\0');
            std::size_t have = 0;
            while (have < got.size()) {
                const auto n = ::recv(fd, got.data() + have, got.size() - have, 0);
                if (n <= 0) {
                    ADD_FAILURE() << "fake server: peer " << (n == 0 ? "closed" : "timed out") << " while expecting: " << e.bytes;
                    ::close(fd);
                    open = false;
                    return false;
                }
                have += static_cast<std::size_t>(n);
            }
            EXPECT_EQ(got, e.bytes);
            return true;
        }

        static bool do_step(const int fd, bool& open, const send_bytes& s) {
            std::size_t sent = 0;
            while (sent < s.bytes.size()) {
                const auto n = ::send(fd, s.bytes.data() + sent, s.bytes.size() - sent, 0);
                if (n <= 0) {
                    ADD_FAILURE() << "fake server: send failed";
                    ::close(fd);
                    open = false;
                    return false;
                }
                sent += static_cast<std::size_t>(n);
            }
            return true;
        }

        static bool do_step(int, bool&, const sleep_for& s) {
            std::this_thread::sleep_for(s.duration);
            return true;
        }

        static bool do_step(const int fd, bool& open, const close_now&) {
            ::close(fd);
            open = false;
            return true;
        }

        static bool do_step(const int fd, bool& open, const expect_eof&) {
            char byte = 0;
            const auto n = ::recv(fd, &byte, 1, 0);
            EXPECT_EQ(n, 0) << "fake server: expected EOF, got " << (n < 0 ? "a timeout" : "a byte");
            ::close(fd);
            open = false;
            return true;
        }

        std::vector<script> scripts_;
        std::size_t total_ = 0;
        std::atomic<std::size_t> ran_{0};
        std::atomic<bool> finished_{false};
        std::atomic<bool> stop_{false};
        int listener_ = -1;
        std::uint16_t port_ = 0;
        std::thread thread_;
    };
}
