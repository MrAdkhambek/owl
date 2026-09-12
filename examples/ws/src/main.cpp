// The three .ws spellings on one server: a coroutine echo, a room that
// owns its path parameter (the request is gone at 101), and a shared
// Chat controller that GET /chat also serves as HTML.

#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <owl/owl.h>

namespace {
    struct App final {};

    coro::task<void> echo(owl::ws::Socket sock, owl::Path<"user", std::string> user) {
        co_await sock.send(std::format("hello {}", user.value));
        while (const auto msg = co_await sock.recv()) {
            co_await sock.send(std::format("{}: {}", user.value, msg->data()), msg->opcode());
        }
    }

    coro::task<void> room(owl::ws::Socket sock, owl::Path<"id", int> id) {
        co_await sock.send(std::format("room {}", id.value));
        while (const auto msg = co_await sock.recv()) {
            co_await sock.send(std::format("{}: {}", id.value, msg->data()), msg->opcode());
        }
    }

    struct Chat final {
        using Extractors = std::tuple<owl::Path<"room", std::string>>;

        coro::task<void> on_connect(const owl::ws::Socket sock, owl::Path<"room", std::string> room) {
            const auto lock = co_await mu_.scoped_lock();
            peers_.insert_or_assign(sock, std::move(room.value));
        }

        coro::task<void> on_message(const owl::ws::Socket, owl::ws::Message msg, const owl::Path<"room", std::string> room) {
            std::vector<owl::ws::Socket> peers;
            {
                const auto lock = co_await mu_.scoped_lock();
                for (const auto& [peer, name] : peers_) {
                    if (name == room.value) peers.push_back(peer);
                }
            }
            const auto opcode = msg.opcode();
            const auto payload = std::format("{}: {}", room.value, std::move(msg).data());
            for (const auto peer : peers) {
                co_await peer.send(std::string{payload}, opcode);
            }
        }

        coro::task<void> on_disconnect(const owl::ws::Socket sock) {
            const auto lock = co_await mu_.scoped_lock();
            peers_.erase(sock);
        }

    private:
        coro::async_mutex mu_;
        std::unordered_map<owl::ws::Socket, std::string> peers_;
    };

    constexpr std::string_view chat_page = R"html(<!doctype html>
<meta charset="utf-8">
<title>owl chat</title>
<pre id="log"></pre>
<input id="msg" autofocus>
<script>
const log = document.getElementById("log");
const ws = new WebSocket((location.protocol === "https:" ? "wss:" : "ws:") + "//" + location.host + "/chat/lobby");
ws.onmessage = (e) => { log.textContent += e.data + "\n"; };
document.getElementById("msg").onkeydown = (e) => {
  if (e.key === "Enter" && e.target.value) { ws.send(e.target.value); e.target.value = ""; }
};
</script>
)html";

    owl::Response page() {
        return owl::Response::body(chat_page, "text/html; charset=utf-8");
    }
}

int main(int argc, char** argv) {
    const auto cfg = owl::Config::make(argc, argv);

    auto router = owl::Router<App>::make()
                  .route<"/chat">(owl::get(page))
                  .ws<"/chat/{room}", Chat>()
                  .ws<"/rooms/{id}">(room)
                  .ws<"/echo/{user}">(echo);

    const owl::Server<App> server = owl::Server<App>::builder()
                                    .router(std::move(router))
                                    .config(cfg)
                                    .build_with(std::make_shared<App>());

    std::printf("listening on http://%s:%u\n  GET  /chat\n  WS   /chat/{room}\n  WS   /rooms/{id}\n  WS   /echo/{user}\n",
                cfg.address.c_str(), server.port());
    std::fflush(stdout);
    server.start();
}
