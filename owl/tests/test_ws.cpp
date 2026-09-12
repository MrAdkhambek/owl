#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <h2o.h>

#include <coro/task.h>

#include <owl/detail.h>
#include <owl/http/response.h>
#include <owl/routing/router.h>
#include <owl/ws/socket.h>

namespace {
    struct App final {
    };

    [[nodiscard]] int listen_loopback() {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) throw std::runtime_error("listen_loopback: socket");
        constexpr int reuse = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 128) != 0) {
            ::close(fd);
            throw std::runtime_error("listen_loopback: bind");
        }
        return fd;
    }

    // Server::start() never returns and has no stop yet, so the live tests
    // build workers from Server's own parts and pump each on a thread they
    // can stop. Each worker listens on its own port, so a test chooses the
    // worker a client lands on.
    struct LiveWorker final {
        owl::MiddlewareChain<App> layers;
        owl::detail::GlobalConf globalconf;
        std::vector<std::unique_ptr<owl::detail::Worker>> workers;
        std::atomic<bool> stop{false};
        std::vector<std::thread> pumps;
        std::vector<std::uint16_t> ports;
        std::uint16_t port = 0;

        explicit LiveWorker(const owl::Router<App>& router, const std::size_t count = 1) {
            std::signal(SIGPIPE, SIG_IGN);
            auto* const host = h2o_config_register_host(&globalconf.conf, h2o_iovec_init(H2O_STRLIT("default")), 65535);
            auto* const path = h2o_config_register_path(host, "/", 0);
            (void)owl::detail::make_dispatcher<App>(path, &router, &layers, std::make_shared<App>(), {});
            for (std::size_t i = 0; i < count; ++i) {
                auto& worker = *workers.emplace_back(std::make_unique<owl::detail::Worker>(&globalconf.conf));
                const int fd = listen_loopback();
                ports.push_back(owl::detail::port_of(fd));
                worker.listener = h2o_evloop_socket_create(worker.ctx.loop, fd, H2O_SOCKET_FLAG_DONT_READ);
                worker.listener->data = &worker;
                h2o_socket_read_start(worker.listener, &owl::detail::on_accept);
            }
            port = ports.front();
            for (const auto& worker : workers) {
                pumps.emplace_back([this, loop = worker->ctx.loop] {
                    while (!stop.load()) h2o_evloop_run(loop, 5);
                });
            }
        }

        ~LiveWorker() {
            stop.store(true);
            for (auto& pump : pumps) pump.join();
            workers.clear();
        }
    };

    struct Http final {
        int status = 0;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;

        [[nodiscard]] std::size_t connection_count() const {
            std::size_t n = 0;
            for (const auto& [name, value] : headers) {
                if (name == "connection") ++n;
            }
            return n;
        }

        [[nodiscard]] std::string header(const std::string_view name) const {
            for (const auto& [key, value] : headers) {
                if (key == name) return value;
            }
            return {};
        }
    };

    struct Client final {
        int fd = -1;
        std::string buf;

        explicit Client(const std::uint16_t port) {
            fd = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) throw std::runtime_error("client: socket");
            timeval tv{.tv_sec = 5, .tv_usec = 0};
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(port);
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
                ::close(fd);
                fd = -1;
                throw std::runtime_error("client: connect");
            }
        }

        ~Client() {
            if (fd >= 0) ::close(fd);
        }

        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        void send_all(const std::string_view data) const {
            std::size_t off = 0;
            while (off < data.size()) {
                const auto n = ::send(fd, data.data() + off, data.size() - off, 0);
                if (n <= 0) return;
                off += static_cast<std::size_t>(n);
            }
        }

        [[nodiscard]] bool fill(const std::size_t n) {
            while (buf.size() < n) {
                char tmp[1024];
                const auto r = ::recv(fd, tmp, sizeof(tmp), 0);
                if (r <= 0) return false;
                buf.append(tmp, static_cast<std::size_t>(r));
            }
            return true;
        }

        Http read_http() {
            Http out;
            while (buf.find("\r\n\r\n") == std::string::npos) {
                if (!fill(buf.size() + 1)) return out;
            }
            const auto end = buf.find("\r\n\r\n");
            const auto head = buf.substr(0, end);
            buf.erase(0, end + 4);
            auto line_end = head.find("\r\n");
            const auto status_line = head.substr(0, line_end);
            const auto sp = status_line.find(' ');
            if (sp != std::string::npos) out.status = std::stoi(status_line.substr(sp + 1, 3));
            std::size_t i = line_end == std::string::npos ? head.size() : line_end + 2;
            while (i < head.size()) {
                const auto nl = head.find("\r\n", i);
                const auto line = head.substr(i, nl - i);
                const auto colon = line.find(':');
                if (colon != std::string::npos) {
                    auto name = line.substr(0, colon);
                    auto value = line.substr(colon + 1);
                    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(0, 1);
                    for (char& c : name) {
                        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                    }
                    out.headers.emplace_back(std::move(name), std::move(value));
                }
                if (nl == std::string::npos) break;
                i = nl + 2;
            }
            std::size_t content_length = 0;
            bool has_length = false;
            for (const auto& [name, value] : out.headers) {
                if (name == "content-length") {
                    content_length = static_cast<std::size_t>(std::stoul(value));
                    has_length = true;
                }
            }
            if (has_length) {
                if (!fill(content_length)) return out;
                out.body = buf.substr(0, content_length);
                buf.erase(0, content_length);
            }
            return out;
        }

        // An empty key leaves the header out.
        Http handshake(const std::string_view path, const std::string_view key = "dGhlIHNhbXBsZSBub25jZQ==",
                       const std::string_view version = "13") {
            std::string head = std::format(
                "GET {} HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n",
                path);
            if (!key.empty()) head += std::format("Sec-WebSocket-Key: {}\r\n", key);
            head += std::format("Sec-WebSocket-Version: {}\r\n\r\n", version);
            send_all(head);
            return read_http();
        }

        Http get(const std::string_view path) {
            send_all(std::format(
                "GET {} HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Connection: close\r\n"
                "\r\n",
                path));
            return read_http();
        }

        [[nodiscard]] static std::string frame(const std::uint8_t opcode, const std::string_view payload) {
            std::string out;
            out.push_back(static_cast<char>(0x80 | opcode));
            constexpr std::array<unsigned char, 4> mask{0x37, 0xfa, 0x21, 0x3d};
            const auto n = payload.size();
            if (n < 126) {
                out.push_back(static_cast<char>(0x80 | n));
            } else {
                out.push_back(static_cast<char>(0x80 | 126));
                out.push_back(static_cast<char>(n >> 8));
                out.push_back(static_cast<char>(n));
            }
            out.append(reinterpret_cast<const char*>(mask.data()), mask.size());
            for (std::size_t i = 0; i < n; ++i) {
                out.push_back(static_cast<char>(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]));
            }
            return out;
        }

        void send_frame(const std::uint8_t opcode, const std::string_view payload) const {
            send_all(frame(opcode, payload));
        }

        [[nodiscard]] bool read_frame(std::uint8_t& opcode, std::string& payload) {
            if (!fill(2)) return false;
            opcode = static_cast<std::uint8_t>(buf[0] & 0x0f);
            const auto masked = (static_cast<unsigned char>(buf[1]) & 0x80) != 0;
            std::size_t len = static_cast<unsigned char>(buf[1]) & 0x7f;
            std::size_t off = 2;
            if (len == 126) {
                if (!fill(4)) return false;
                len = (static_cast<unsigned char>(buf[2]) << 8) | static_cast<unsigned char>(buf[3]);
                off = 4;
            }
            if (masked) return false;
            if (!fill(off + len)) return false;
            payload.assign(buf.data() + off, len);
            buf.erase(0, off + len);
            return true;
        }

        void send_close() {
            const char body[] = {static_cast<char>(0x03), static_cast<char>(0xe8)};
            send_frame(0x8, std::string_view{body, 2});
        }

        void read_close_and_eof() {
            std::uint8_t opcode = 0;
            std::string payload;
            EXPECT_TRUE(read_frame(opcode, payload));
            EXPECT_EQ(opcode, 0x8);
            for (;;) {
                char tmp[64];
                const auto n = ::recv(fd, tmp, sizeof(tmp), 0);
                if (n == 0) return;
                if (n < 0) {
                    EXPECT_TRUE(false);
                    return;
                }
            }
        }

        // The close frame's status code, then EOF; 0 if no close frame came.
        [[nodiscard]] std::uint16_t read_close_code() {
            std::uint8_t opcode = 0;
            std::string payload;
            if (!read_frame(opcode, payload) || opcode != 0x8 || payload.size() < 2) return 0;
            for (;;) {
                char tmp[64];
                if (::recv(fd, tmp, sizeof(tmp), 0) <= 0) break;
            }
            return static_cast<std::uint16_t>((static_cast<unsigned char>(payload[0]) << 8) | static_cast<unsigned char>(payload[1]));
        }
    };

    coro::task<void> echo(owl::ws::Socket sock) {
        while (const auto msg = co_await sock.recv()) {
            co_await sock.send(std::string{msg->data()}, msg->opcode());
        }
    }

    coro::task<void> bye(owl::ws::Socket sock) {
        co_await sock.send("bye");
    }

    std::atomic<bool> peer_gone{false};

    coro::task<void> vanish(owl::ws::Socket sock) {
        const auto msg = co_await sock.recv();
        peer_gone.store(!msg.has_value());
    }

    owl::Response page() {
        return owl::Response::ok("html");
    }

    struct SharedEcho final {
        std::atomic<int> connections{0};

        void on_connect(owl::ws::Socket) {
            connections.fetch_add(1);
        }

        coro::task<void> on_message(owl::ws::Socket sock, owl::ws::Message msg) {
            co_await sock.send(std::string{msg.data()}, msg.opcode());
        }
    };

    coro::task<owl::Response> tag_layer(const owl::Request& req, owl::Next<App> next) {
        auto res = co_await next(req);
        res.header("x-layer", "1");
        co_return std::move(res);
    }
}

TEST(Ws, EchoRoundTrip) {
    auto router = owl::Router<App>::make().ws<"/echo">(echo);
    LiveWorker worker{router};
    Client client{worker.port};
    const auto hs = client.handshake("/echo");
    EXPECT_EQ(hs.status, 101);
    EXPECT_EQ(hs.header("sec-websocket-accept"), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    EXPECT_EQ(hs.connection_count(), 1u);
    if (hs.status != 101) return;
    client.send_frame(0x1, "hi");
    std::uint8_t opcode = 0;
    std::string payload;
    EXPECT_TRUE(client.read_frame(opcode, payload));
    EXPECT_EQ(opcode, 0x1);
    EXPECT_EQ(payload, "hi");
    client.send_close();
    client.read_close_and_eof();
}

TEST(Ws, HandlerReturnDrainsItsLastSend) {
    auto router = owl::Router<App>::make().ws<"/bye">(bye);
    LiveWorker worker{router};
    Client client{worker.port};
    const auto hs = client.handshake("/bye");
    EXPECT_EQ(hs.status, 101);
    if (hs.status != 101) return;
    std::uint8_t opcode = 0;
    std::string payload;
    EXPECT_TRUE(client.read_frame(opcode, payload));
    EXPECT_EQ(opcode, 0x1);
    EXPECT_EQ(payload, "bye");
    client.read_close_and_eof();
}

TEST(Ws, PeerVanishingEndsTheHandler) {
    peer_gone.store(false);
    auto router = owl::Router<App>::make().ws<"/vanish">(vanish);
    LiveWorker worker{router};
    {
        Client client{worker.port};
        const auto hs = client.handshake("/vanish");
        EXPECT_EQ(hs.status, 101);
        if (hs.status != 101) return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!peer_gone.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(peer_gone.load());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

TEST(Ws, MiddlewareHeadersRideOnThe101) {
    auto router = owl::Router<App>::make()
                      .layer(tag_layer)
                      .ws<"/echo">(echo);
    LiveWorker worker{router};
    Client client{worker.port};
    const auto hs = client.handshake("/echo");
    EXPECT_EQ(hs.status, 101);
    EXPECT_EQ(hs.header("x-layer"), "1");
    if (hs.status != 101) return;
    client.send_close();
    client.read_close_and_eof();
}

TEST(Ws, PlainGetOnWsOnlyPathIs404) {
    auto router = owl::Router<App>::make().ws<"/echo">(echo);
    LiveWorker worker{router};
    Client client{worker.port};
    const auto res = client.get("/echo");
    EXPECT_EQ(res.status, 404);
}

TEST(Ws, GetAndWsShareAPath) {
    auto router = owl::Router<App>::make()
                      .route<"/chat">(owl::get(page))
                      .ws<"/chat">(echo);
    LiveWorker worker{router};
    {
        Client client{worker.port};
        const auto res = client.get("/chat");
        EXPECT_EQ(res.status, 200);
        EXPECT_EQ(res.body, "html");
    }
    Client client{worker.port};
    const auto hs = client.handshake("/chat");
    EXPECT_EQ(hs.status, 101);
    if (hs.status != 101) return;
    client.send_close();
    client.read_close_and_eof();
}

TEST(Ws, BadKeyIs400) {
    auto router = owl::Router<App>::make().ws<"/echo">(echo);
    LiveWorker worker{router};
    Client client{worker.port};
    const auto hs = client.handshake("/echo", "short");
    EXPECT_EQ(hs.status, 400);
}

TEST(Ws, SharedControllerEchoesAcrossTwoConnections) {
    const auto echo = std::make_shared<SharedEcho>();
    auto router = owl::Router<App>::make().ws<"/echo", SharedEcho>(echo);
    LiveWorker worker{router};
    for (int i = 0; i < 2; ++i) {
        Client client{worker.port};
        const auto hs = client.handshake("/echo");
        EXPECT_EQ(hs.status, 101);
        if (hs.status != 101) return;
        client.send_frame(0x1, "hi");
        std::uint8_t opcode = 0;
        std::string payload;
        EXPECT_TRUE(client.read_frame(opcode, payload));
        EXPECT_EQ(opcode, 0x1);
        EXPECT_EQ(payload, "hi");
        client.send_close();
        client.read_close_and_eof();
    }
    EXPECT_EQ(echo->connections.load(), 2);
}

namespace {
    template <typename Pred>
    bool wait_for(Pred pred, const std::chrono::milliseconds limit = std::chrono::seconds(5)) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (!pred()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }

    struct Faulty final {
        std::atomic<int> connected{0};
        std::atomic<int> disconnected{0};

        void on_connect(owl::ws::Socket) {
            connected.fetch_add(1);
        }

        void on_message(owl::ws::Socket, owl::ws::Message) {
            throw std::runtime_error("boom");
        }

        void on_disconnect(owl::ws::Socket) {
            disconnected.fetch_add(1);
        }
    };

    coro::task<void> boom(owl::ws::Socket sock) {
        (void)co_await sock.recv();
        throw std::runtime_error("boom");
    }
}

TEST(Ws, ThrowingControllerStillDisconnects) {
    const auto faulty = std::make_shared<Faulty>();
    auto router = owl::Router<App>::make().ws<"/faulty", Faulty>(faulty);
    LiveWorker worker{router};
    Client client{worker.port};
    const auto hs = client.handshake("/faulty");
    EXPECT_EQ(hs.status, 101);
    if (hs.status != 101) return;
    client.send_frame(0x1, "hi");
    EXPECT_EQ(client.read_close_code(), 1011);
    EXPECT_TRUE(wait_for([&] { return faulty->disconnected.load() == 1; }));
    EXPECT_EQ(faulty->connected.load(), 1);
}

TEST(Ws, ThrowingHandlerClosesWith1011) {
    auto router = owl::Router<App>::make().ws<"/boom">(boom);
    LiveWorker worker{router};
    Client client{worker.port};
    const auto hs = client.handshake("/boom");
    EXPECT_EQ(hs.status, 101);
    if (hs.status != 101) return;
    client.send_frame(0x1, "hi");
    EXPECT_EQ(client.read_close_code(), 1011);
}

TEST(Ws, OtherVersionIs426WithVersion13) {
    auto router = owl::Router<App>::make().ws<"/echo">(echo);
    LiveWorker worker{router};
    Client client{worker.port};
    const auto hs = client.handshake("/echo", "dGhlIHNhbXBsZSBub25jZQ==", "8");
    EXPECT_EQ(hs.status, 426);
    EXPECT_EQ(hs.header("sec-websocket-version"), "13");
}

TEST(Ws, MissingKeyIs400) {
    auto router = owl::Router<App>::make().ws<"/echo">(echo);
    LiveWorker worker{router};
    Client client{worker.port};
    EXPECT_EQ(client.handshake("/echo", "").status, 400);
}

namespace {
    struct Kicker final {
        std::mutex mu;
        std::vector<owl::ws::Socket> peers;
        std::atomic<int> joined{0};
        std::atomic<int> left{0};
        std::atomic<bool> closing_others{false};
        std::atomic<bool> nested{false};

        void on_connect(owl::ws::Socket sock) {
            {
                const std::lock_guard lock{mu};
                peers.push_back(sock);
            }
            joined.fetch_add(1);
        }

        void on_message(owl::ws::Socket from, owl::ws::Message) {
            std::vector<owl::ws::Socket> copy;
            {
                const std::lock_guard lock{mu};
                copy = peers;
            }
            closing_others.store(true);
            for (const auto& peer : copy) {
                if (peer != from) peer.close();
            }
            closing_others.store(false);
        }

        void on_disconnect(owl::ws::Socket sock) {
            if (closing_others.load()) nested.store(true);
            {
                const std::lock_guard lock{mu};
                std::erase(peers, sock);
            }
            left.fetch_add(1);
        }
    };
}

TEST(Ws, ClosingAnotherConnectionDoesNotRunItsHandlerInline) {
    const auto kicker = std::make_shared<Kicker>();
    auto router = owl::Router<App>::make().ws<"/kick", Kicker>(kicker);
    LiveWorker worker{router};
    Client a{worker.port};
    Client b{worker.port};
    EXPECT_EQ(a.handshake("/kick").status, 101);
    EXPECT_EQ(b.handshake("/kick").status, 101);
    ASSERT_TRUE(wait_for([&] { return kicker->joined.load() == 2; }));
    a.send_frame(0x1, "kick");
    b.read_close_and_eof();
    EXPECT_TRUE(wait_for([&] { return kicker->left.load() == 1; }));
    EXPECT_FALSE(kicker->nested.load());
    a.send_close();
    a.read_close_and_eof();
}

namespace {
    struct Broadcast final {
        std::mutex mu;
        std::vector<owl::ws::Socket> peers;
        std::atomic<int> joined{0};
        std::atomic<int> left{0};

        void on_connect(owl::ws::Socket sock) {
            {
                const std::lock_guard lock{mu};
                peers.push_back(sock);
            }
            joined.fetch_add(1);
        }

        void on_message(owl::ws::Socket from, owl::ws::Message msg) {
            std::vector<owl::ws::Socket> copy;
            {
                const std::lock_guard lock{mu};
                copy = peers;
            }
            for (const auto& peer : copy) {
                if (peer != from) (void)peer.send(std::string{msg.data()}, msg.opcode());
            }
        }

        void on_disconnect(owl::ws::Socket sock) {
            {
                const std::lock_guard lock{mu};
                std::erase(peers, sock);
            }
            left.fetch_add(1);
        }
    };

    // Keeps the Socket past on_disconnect on purpose: the bug a controller
    // with a missed erase has.
    struct Keeper final {
        std::mutex mu;
        std::optional<owl::ws::Socket> kept;
        std::atomic<bool> ended{false};

        void on_connect(owl::ws::Socket sock) {
            const std::lock_guard lock{mu};
            kept.emplace(sock);
        }

        void on_message(owl::ws::Socket, owl::ws::Message) {}

        void on_disconnect(owl::ws::Socket) {
            ended.store(true);
        }
    };
}

TEST(Ws, BroadcastReachesAPeerOnAnotherWorker) {
    const auto hub = std::make_shared<Broadcast>();
    auto router = owl::Router<App>::make().ws<"/hub", Broadcast>(hub);
    LiveWorker workers{router, 2};
    Client a{workers.ports[0]};
    Client b{workers.ports[1]};
    EXPECT_EQ(a.handshake("/hub").status, 101);
    EXPECT_EQ(b.handshake("/hub").status, 101);
    ASSERT_TRUE(wait_for([&] { return hub->joined.load() == 2; }));
    a.send_frame(0x1, "hi");
    std::uint8_t opcode = 0;
    std::string payload;
    EXPECT_TRUE(b.read_frame(opcode, payload));
    EXPECT_EQ(payload, "hi");
    a.send_close();
    a.read_close_and_eof();
    b.send_close();
    b.read_close_and_eof();
    EXPECT_TRUE(wait_for([&] { return hub->left.load() == 2; }));
}

TEST(Ws, SocketKeptPastItsConnectionIsInert) {
    const auto keeper = std::make_shared<Keeper>();
    auto router = owl::Router<App>::make().ws<"/keep", Keeper>(keeper);
    LiveWorker worker{router};
    {
        Client client{worker.port};
        const auto hs = client.handshake("/keep");
        EXPECT_EQ(hs.status, 101);
        if (hs.status != 101) return;
        client.send_close();
        client.read_close_and_eof();
    }
    ASSERT_TRUE(wait_for([&] { return keeper->ended.load(); }));
    // The session is reaped on the loop pass after the handler returns.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::lock_guard lock{keeper->mu};
    ASSERT_TRUE(keeper->kept.has_value());
    EXPECT_FALSE(keeper->kept->open());
    (void)keeper->kept->send("late");
    keeper->kept->close();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

namespace {
    // Lowers the engine's limits for one test. Declare it before the
    // LiveWorker, so the workers start after the write and stop before the
    // restore.
    struct LimitsGuard final {
        owl::ws::detail::Limits saved = owl::ws::detail::default_limits;

        explicit LimitsGuard(const owl::ws::detail::Limits next) {
            owl::ws::detail::default_limits = next;
        }

        ~LimitsGuard() {
            owl::ws::detail::default_limits = saved;
        }

        LimitsGuard(const LimitsGuard&) = delete;
        LimitsGuard& operator=(const LimitsGuard&) = delete;
    };

    std::atomic<int> flood_sent{-1};

    coro::task<void> flood(owl::ws::Socket sock) {
        const std::string chunk(64 * 1024, 'x');
        int sent = 0;
        while (sock.open() && sent < 1024) {
            co_await sock.send(std::string{chunk}, owl::ws::Opcode::Binary);
            ++sent;
        }
        flood_sent.store(sent);
    }

    std::atomic<bool> stall_done{false};

    coro::task<void> stall(owl::ws::Socket sock, const owl::loop_scheduler& loop) {
        co_await loop.schedule_after(std::chrono::milliseconds(1500));
        while (co_await sock.recv()) {
        }
        stall_done.store(true);
    }
}

TEST(Ws, UnreadSendsPastTheCapDropTheConnection) {
    const LimitsGuard guard{{.max_unsent_bytes = 256 * 1024}};
    flood_sent.store(-1);
    auto router = owl::Router<App>::make().ws<"/flood">(flood);
    LiveWorker worker{router};
    Client client{worker.port};
    ASSERT_EQ(client.handshake("/flood").status, 101);
    ASSERT_TRUE(wait_for([] { return flood_sent.load() >= 0; }));
    EXPECT_LT(flood_sent.load(), 1024);
}

// The handler sleeps before its first recv(); a peer that keeps writing
// should be held back by TCP once max_pending_bytes is waiting, not read
// into memory.
TEST(Ws, UnreadBacklogPausesReading) {
    const LimitsGuard guard{{.max_pending_bytes = 64 * 1024}};
    stall_done.store(false);
    auto router = owl::Router<App>::make().ws<"/stall">(stall);
    LiveWorker worker{router};
    {
        Client client{worker.port};
        ASSERT_EQ(client.handshake("/stall").status, 101);
        ::fcntl(client.fd, F_SETFL, ::fcntl(client.fd, F_GETFL) | O_NONBLOCK);
        const std::string frame = Client::frame(0x2, std::string(16 * 1024, 'x'));
        constexpr std::size_t flood_bytes = 64u * 1024 * 1024;
        std::size_t accepted = 0;
        std::size_t off = 0;
        auto progress = std::chrono::steady_clock::now();
        while (accepted < flood_bytes
               && std::chrono::steady_clock::now() - progress < std::chrono::milliseconds(300)) {
            const auto n = ::send(client.fd, frame.data() + off, frame.size() - off, 0);
            if (n > 0) {
                accepted += static_cast<std::size_t>(n);
                off = (off + static_cast<std::size_t>(n)) % frame.size();
                progress = std::chrono::steady_clock::now();
            } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } else {
                break;
            }
        }
        EXPECT_LT(accepted, flood_bytes / 2);
    }
    EXPECT_TRUE(wait_for([] { return stall_done.load(); }, std::chrono::seconds(10)));
}

TEST(Ws, SilentPeerIsPingedThenDropped) {
    const LimitsGuard guard{{.idle_ping_ms = 100}};
    peer_gone.store(false);
    auto router = owl::Router<App>::make().ws<"/vanish">(vanish);
    LiveWorker worker{router};
    Client client{worker.port};
    ASSERT_EQ(client.handshake("/vanish").status, 101);
    std::uint8_t opcode = 0;
    std::string payload;
    EXPECT_TRUE(client.read_frame(opcode, payload));
    EXPECT_EQ(opcode, 0x9);
    EXPECT_TRUE(wait_for([] { return peer_gone.load(); }));
}

TEST(Ws, PongKeepsAQuietPeerConnected) {
    const LimitsGuard guard{{.idle_ping_ms = 100}};
    auto router = owl::Router<App>::make().ws<"/echo">(echo);
    LiveWorker worker{router};
    Client client{worker.port};
    ASSERT_EQ(client.handshake("/echo").status, 101);
    for (int i = 0; i < 5; ++i) {
        std::uint8_t opcode = 0;
        std::string payload;
        ASSERT_TRUE(client.read_frame(opcode, payload));
        ASSERT_EQ(opcode, 0x9);
        client.send_frame(0xA, payload);
    }
    client.send_frame(0x1, "still here");
    std::uint8_t opcode = 0;
    std::string payload;
    // A ping may still land ahead of the echo; skip it.
    while (client.read_frame(opcode, payload) && opcode == 0x9) {
    }
    EXPECT_EQ(opcode, 0x1);
    EXPECT_EQ(payload, "still here");
    client.send_close();
    client.read_close_and_eof();
}
