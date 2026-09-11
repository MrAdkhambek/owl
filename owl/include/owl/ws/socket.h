#pragma once

// One WebSocket connection, as a handler sees it.
//
//     coro::task<void> room(owl::ws::Socket sock, owl::Path<"id", int> id) {
//         co_await sock.send("welcome");
//         while (const auto msg = co_await sock.recv()) {
//             co_await sock.send(msg->data());
//         }
//     }                                    // returning closes the connection
//
// The handler *is* the connection: it starts when the upgrade completes and the
// connection closes when it returns, so per-connection state is just locals and
// there is no lifecycle to implement. `recv()` returning nullopt is the peer
// having gone away.
//
// Exposes no wslay type. Engine-only operations go through SessionOps, and the
// accessors that only read Session are inline in session.h so every translation
// unit that holds a handler links.

#include <coroutine>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "owl/ws/detail/session.h"
#include "owl/ws/message.h"

namespace owl::ws {
    class Socket final {
    public:
        explicit Socket(detail::Session* session) noexcept : session_(session) {
        }

        // Suspends until the next message, or resumes with nullopt once the
        // peer has gone. `while (const auto m = co_await sock.recv())` is the
        // whole read loop.
        class Recv final {
        public:
            explicit Recv(detail::Session* session) noexcept : session_(session) {
            }

            // A queued message, or an already-closed connection, needs no trip
            // through the event loop.
            [[nodiscard]] bool await_ready() const noexcept {
                return detail::has_message(session_) || !detail::is_open(session_);
            }

            void await_suspend(const std::coroutine_handle<> waiter) const noexcept {
                detail::park(session_, waiter);
            }

            [[nodiscard]] std::optional<Message> await_resume() const noexcept {
                return detail::take_message(session_);
            }

        private:
            detail::Session* session_;
        };

        // Queued and flushed immediately. Awaitable but never actually
        // suspends: wslay buffers, so there is nothing to wait for yet.
        // Spelled as an awaitable anyway so that adding real backpressure later
        // does not change a single call site.
        class Send final {
        public:
            [[nodiscard]] bool await_ready() const noexcept { return true; }

            void await_suspend(std::coroutine_handle<>) const noexcept {
            }

            void await_resume() const noexcept {
            }
        };

        [[nodiscard]] Recv recv() const noexcept { return Recv{session_}; }

        // One overload on purpose. A string_view companion looked like a free
        // optimisation and was really an ambiguity: passing a std::string made
        // both viable in enough contexts to be a nuisance, and the payload has
        // to be owned here anyway -- wslay copies it, but not until the queue
        // is flushed. A literal or a view converts on the way in.
        [[nodiscard]] Send send(std::string data, const Opcode opcode = Opcode::Text) const noexcept {
            detail::enqueue(session_, std::move(data), opcode);
            return Send{};
        }

        // Starts the closing handshake. The handler's next recv() reports
        // nullopt; returning from the handler does this anyway.
        void close() const noexcept { detail::begin_close(session_); }

        [[nodiscard]] bool open() const noexcept { return detail::is_open(session_); }

        // A stable identity for the connection, valid until it closes.
        //
        // A shared controller sees every connection through the same instance,
        // so it needs something to key per-connection state on; this is it.
        // Opaque on purpose -- it is the session address, and nothing but
        // comparison and hashing is meaningful.
        [[nodiscard]] const void* id() const noexcept { return session_; }

        [[nodiscard]] friend bool operator==(const Socket a, const Socket b) noexcept {
            return a.session_ == b.session_;
        }

        // There is deliberately no request() accessor. The h2o request is gone
        // the moment the upgrade completes, and a Request holds a pointer to
        // the driver plus path parameters that are string_views into h2o's
        // path buffer -- every one of them dangling by the time a handler could
        // read them. Ask for what you need as a handler parameter instead,
        // where it is extracted before the upgrade and copied into the frame.
        // Owning extractors only; see the static_assert in Router::ws.

    private:
        detail::Session* session_;
    };
}

// So a controller can write unordered_map<Socket, ...> directly.
template <>
struct std::hash<owl::ws::Socket> {
    std::size_t operator()(const owl::ws::Socket sock) const noexcept {
        return std::hash<const void*>{}(sock.id());
    }
};
