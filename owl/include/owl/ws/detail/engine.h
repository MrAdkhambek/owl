#pragma once

/*
 * Copyright (c) 2014 DeNA Co., Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

// The only header that sees wslay. Public owl/ws/*.h stay wslay-free, and
// the h2o request is gone once the upgrade completes -- nothing the
// connection needs may live in that frame.
//
// Everything here is inline, not static: upgrade() and adopt() are inline,
// and an inline function that names a static one is an ODR violation in
// every translation unit after the first.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <h2o.h>
#include <h2o/http1.h>
#include <openssl/evp.h>
#include <wslay/wslay.h>

#include "coro/task.h"
#include "owl/http/request.h"
#include "owl/ws/detail/handshake.h"
#include "owl/ws/detail/session.h"

namespace owl::ws::detail {
    inline constexpr std::size_t max_message_bytes = 16 * 1024 * 1024;
    inline constexpr char ws_guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    [[nodiscard]] inline wslay_event_context_ptr ctx_of(const Session* session) noexcept {
        return static_cast<wslay_event_context_ptr>(session->wslay);
    }

    inline void free_batch(Session* session) noexcept {
        for (std::size_t i = 0; i < session->batched; ++i) {
            std::free(session->batch[i].base);
            session->batch[i] = {};
        }
        session->batched = 0;
    }

    // Every place the connection stops delivering, so Socket::open() on any
    // thread agrees with the owner.
    inline void mark_closing(Session* session) noexcept {
        session->closing = true;
        session->handle->open.store(false, std::memory_order_relaxed);
    }

    inline void wake(Session* session) noexcept;
    inline void proceed(Session* session) noexcept;
    inline void maybe_reap(Session* session) noexcept;
    inline void destroy(Session* session) noexcept;
    inline void on_recv(h2o_socket_t* sock, const char* err);
    inline void on_write_complete(h2o_socket_t* sock, const char* err);
    inline void finish(Session* session, std::uint16_t code) noexcept;

    inline ssize_t recv_callback(wslay_event_context_ptr ctx, uint8_t* buf, size_t len, int, void* user_data) {
        auto* const session = static_cast<Session*>(user_data);
        if (session->sock->input->size == 0) {
            wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
            return -1;
        }
        if (session->sock->input->size < len) len = session->sock->input->size;
        std::memcpy(buf, session->sock->input->bytes, len);
        h2o_buffer_consume(&session->sock->input, len);
        return static_cast<ssize_t>(len);
    }

    inline ssize_t send_callback(wslay_event_context_ptr ctx, const uint8_t* data, size_t len, int, void* user_data) {
        auto* const session = static_cast<Session*>(user_data);
        if (h2o_socket_is_writing(session->sock) || session->batched == session->batch.size()) {
            wslay_event_set_error(ctx, WSLAY_ERR_WOULDBLOCK);
            return -1;
        }
        auto& buf = session->batch[session->batched];
        buf.base = static_cast<char*>(h2o_mem_alloc(len));
        buf.len = len;
        std::memcpy(buf.base, data, len);
        ++session->batched;
        return static_cast<ssize_t>(len);
    }

    inline void on_msg_recv(wslay_event_context_ptr, const struct wslay_event_on_msg_recv_arg* arg, void* user_data) {
        if (arg->opcode != WSLAY_TEXT_FRAME && arg->opcode != WSLAY_BINARY_FRAME) return;
        auto* const session = static_cast<Session*>(user_data);
        session->pending.emplace_back(
            std::string(reinterpret_cast<const char*>(arg->msg), arg->msg_length),
            arg->opcode == WSLAY_BINARY_FRAME ? Opcode::Binary : Opcode::Text);
    }

    inline constexpr wslay_event_callbacks wslay_callbacks{
        .recv_callback = recv_callback,
        .send_callback = send_callback,
        .genmask_callback = nullptr,
        .on_frame_recv_start_callback = nullptr,
        .on_frame_recv_chunk_callback = nullptr,
        .on_frame_recv_end_callback = nullptr,
        .on_msg_recv_callback = on_msg_recv,
    };

    inline void wake(Session* session) noexcept {
        if (const auto waiter = std::exchange(session->waiter, {})) {
            waiter.resume();
        }
    }

    inline void destroy(Session* session) noexcept {
        if (h2o_timer_is_linked(&session->kicker.timer)) h2o_timer_unlink(&session->kicker.timer);
        if (session->sock != nullptr) {
            h2o_socket_close(session->sock);
            session->sock = nullptr;
        }
        free_batch(session);
        if (session->wslay != nullptr) {
            wslay_event_context_free(ctx_of(session));
            session->wslay = nullptr;
        }
        delete session;
    }

    inline void on_reap(h2o_timer_t* entry) {
        destroy(H2O_STRUCT_FROM_MEMBER(SessionTimer, timer, entry)->session);
    }

    inline void maybe_reap(Session* session) noexcept {
        if (!session->finished) return;
        const bool idle = session->dead
            || session->sock == nullptr
            || (!h2o_socket_is_writing(session->sock)
                && session->batched == 0
                && !wslay_event_want_write(ctx_of(session)));
        if (!idle) return;
        if (!h2o_timer_is_linked(&session->reaper.timer)) {
            h2o_timer_link(session->loop, 0, &session->reaper.timer);
        }
    }

    // Everything that may resume this session's handler, deferred to the
    // loop's next pass on the session's own timer, so it starts from the
    // loop rather than inside whoever asked.
    inline void on_kick(h2o_timer_t* entry) {
        auto* const session = H2O_STRUCT_FROM_MEMBER(SessionTimer, timer, entry)->session;
        if (session->sock != nullptr && !session->dead) {
            proceed(session);
        } else {
            wake(session);
        }
        maybe_reap(session);
    }

    inline void kick(Session* session) noexcept {
        if (!h2o_timer_is_linked(&session->kicker.timer)) h2o_timer_link(session->loop, 0, &session->kicker.timer);
    }

    // The write half of proceed() and nothing else: no read, no wake. So it
    // is safe from inside any handler, including another connection's --
    // which is exactly where a broadcast calls it.
    inline void flush(Session* session) noexcept {
        if (session->sock == nullptr || session->dead || h2o_socket_is_writing(session->sock)) return;
        auto* const ctx = ctx_of(session);
        if (wslay_event_want_write(ctx) && wslay_event_send(ctx) != 0) {
            mark_closing(session);
            kick(session);
            return;
        }
        if (session->batched > 0) {
            h2o_socket_write(session->sock, session->batch.data(), session->batched, on_write_complete);
        }
    }

    inline void proceed(Session* session) noexcept {
        auto* const ctx = ctx_of(session);
        bool close = false;
        int handled = 0;
        do {
            handled = 0;
            if (!h2o_socket_is_writing(session->sock) && wslay_event_want_write(ctx)) {
                if (wslay_event_send(ctx) != 0) {
                    close = true;
                    break;
                }
                if (session->batched < session->batch.size()) {
                    handled = 1;
                }
            }
            if (session->sock->input->size != 0 && wslay_event_want_read(ctx)) {
                if (wslay_event_recv(ctx) != 0) {
                    close = true;
                    break;
                }
                handled = 1;
            }
        } while (handled);

        if (!close) {
            if (!h2o_socket_is_writing(session->sock) && session->batched > 0) {
                h2o_socket_write(session->sock, session->batch.data(), session->batched, on_write_complete);
            }
            if (wslay_event_want_read(ctx)) {
                h2o_socket_read_start(session->sock, on_recv);
            } else if (h2o_socket_is_writing(session->sock) || wslay_event_want_write(ctx)) {
                h2o_socket_read_stop(session->sock);
            } else {
                close = true;
            }
        }
        if (close) {
            mark_closing(session);
            h2o_socket_read_stop(session->sock);
        }
        if (!session->pending.empty() || session->closing) {
            wake(session);
        }
    }

    inline void on_recv(h2o_socket_t* sock, const char* err) {
        auto* const session = static_cast<Session*>(sock->data);
        if (err != nullptr) {
            mark_closing(session);
            session->dead = true;
            h2o_socket_read_stop(sock);
            wake(session);
            maybe_reap(session);
            return;
        }
        proceed(session);
    }

    inline void on_write_complete(h2o_socket_t* sock, const char* err) {
        auto* const session = static_cast<Session*>(sock->data);
        free_batch(session);
        if (err != nullptr) {
            mark_closing(session);
            session->dead = true;
            wake(session);
        } else {
            proceed(session);
        }
        maybe_reap(session);
    }

    // The handler is the connection, so its failure is the connection's:
    // 1011 tells the peer this was not a normal close. Nothing is logged --
    // owl's library code has no logger -- so the close code is the signal.
    inline coro::task<void> supervise(Session* session, coro::task<void> inner) {
        std::uint16_t code = WSLAY_CODE_NORMAL_CLOSURE;
        try {
            co_await std::move(inner);
        } catch (...) {
            code = WSLAY_CODE_INTERNAL_SERVER_ERROR;
        }
        finish(session, code);
    }

    inline void finish(Session* session, const std::uint16_t code) noexcept {
        session->finished = true;
        if (session->sock != nullptr && !session->closing) {
            wslay_event_queue_close(ctx_of(session), code, nullptr, 0);
            mark_closing(session);
        }
        if (session->sock != nullptr) proceed(session);
        maybe_reap(session);
    }

    inline void adopt(Session* session, coro::task<void> handler) noexcept {
        session->handler = supervise(session, std::move(handler));
    }

    // Owner thread only. A failed queue is out of memory or a close already
    // queued; either way nothing more will be delivered, and a handler
    // parked in recv() must hear that -- from its own kick, not from here.
    inline void enqueue_here(Session* session, std::string data, const Opcode opcode) noexcept {
        if (session->closing || session->dead) return;
        const wslay_event_msg message{
            static_cast<uint8_t>(opcode == Opcode::Binary ? WSLAY_BINARY_FRAME : WSLAY_TEXT_FRAME),
            reinterpret_cast<const uint8_t*>(data.data()),
            data.size(),
        };
        if (wslay_event_queue_msg(ctx_of(session), &message) != 0) {
            mark_closing(session);
            kick(session);
            return;
        }
        flush(session);
    }

    inline void close_here(Session* session) noexcept {
        if (session->closing) return;
        mark_closing(session);
        wslay_event_queue_close(ctx_of(session), WSLAY_CODE_NORMAL_CLOSURE, nullptr, 0);
        flush(session);
        kick(session);
    }

    struct Post;

    // Standard-layout, so the receiver can walk back from the list node with
    // H2O_STRUCT_FROM_MEMBER. Post holds a std::string and need not be.
    struct PostHook final {
        h2o_multithread_message_t message{};
        Post* post = nullptr;
    };

    // A send or close from a thread that does not own the connection,
    // carried to the thread that does. It holds a reference, so the Handle
    // survives the trip even when the connection does not.
    struct Post final {
        PostHook hook;
        Handle* handle = nullptr;
        std::string data;
        Opcode opcode = Opcode::Text;
        bool close = false;
    };

    // The owner worker's receiver; detail.h registers it per Context.
    inline void on_post(h2o_multithread_receiver_t*, h2o_linklist_t* const messages) {
        while (!h2o_linklist_is_empty(messages)) {
            auto* const hook = H2O_STRUCT_FROM_MEMBER(PostHook, message.link, messages->next);
            const std::unique_ptr<Post> post{hook->post};
            h2o_linklist_unlink(&hook->message.link);
            if (Session* const session = post->handle->session) {
                if (post->close) {
                    close_here(session);
                } else {
                    enqueue_here(session, std::move(post->data), post->opcode);
                }
            }
            release(post->handle);
        }
    }

    [[nodiscard]] inline bool on_owner(const Handle* handle) noexcept {
        return std::this_thread::get_id() == handle->owner;
    }

    // open is only a cheap early out; the owner checks the session again on
    // delivery, which is the check that counts.
    inline void post_to_owner(Handle* handle, std::string data, const Opcode opcode, const bool close) noexcept {
        if (!handle->open.load(std::memory_order_relaxed)) return;
        retain(handle);
        auto* const post = new Post{.hook = {}, .handle = handle, .data = std::move(data), .opcode = opcode, .close = close};
        post->hook.post = post;
        h2o_multithread_send_message(handle->hop, &post->hook.message);
    }

    inline void engine_enqueue(Handle* handle, std::string data, const Opcode opcode) noexcept {
        if (!on_owner(handle)) {
            post_to_owner(handle, std::move(data), opcode, false);
            return;
        }
        if (Session* const session = handle->session) enqueue_here(session, std::move(data), opcode);
    }

    inline void engine_close(Handle* handle) noexcept {
        if (!on_owner(handle)) {
            post_to_owner(handle, {}, Opcode::Text, true);
            return;
        }
        if (Session* const session = handle->session) close_here(session);
    }

    inline constexpr SessionOps engine_ops{
        .enqueue = &engine_enqueue,
        .close = &engine_close,
    };

    inline void on_complete(void* data, h2o_socket_t* sock, size_t reqsize) {
        auto* const session = static_cast<Session*>(data);
        if (sock == nullptr) {
            destroy(session);
            return;
        }
        session->sock = sock;
        sock->data = session;
        h2o_buffer_consume(&sock->input, reqsize);
        session->handle->open.store(true, std::memory_order_relaxed);
        session->handler.start();
        proceed(session);
    }

    [[nodiscard]] inline bool valid_key(h2o_req_t* req, const std::string_view key) noexcept {
        if (key.size() != 24) return false;
        // `=` is not in the base64url alphabet. A 16-byte nonce is 24 chars
        // with padding; stripping it is what makes decode report 16 bytes
        // rather than fail. The function accepts `+` and `/` too.
        auto b64 = key;
        while (!b64.empty() && b64.back() == '=') b64.remove_suffix(1);
        const auto decoded = h2o_decode_base64url(&req->pool, b64.data(), b64.size());
        return decoded.base != nullptr && decoded.len == 16;
    }

    inline void create_accept_key(char* dst, const char* client_key) {
        unsigned char digest[20];
        unsigned int digest_len = 0;
        unsigned char key_src[60];
        std::memcpy(key_src, client_key, 24);
        std::memcpy(key_src + 24, ws_guid, 36);
        EVP_Digest(key_src, sizeof(key_src), digest, &digest_len, EVP_sha1(), nullptr);
        h2o_base64_encode(dst, digest, sizeof(digest), 0);
        dst[28] = '\0';
    }

    // Returns the status the exchange ended with: 101 once h2o owns the
    // connection, otherwise the error run_handler sends. Takes the Request
    // run_handler already has -- building a second one cost a pool
    // allocation and a query parse per upgrade. Still checks
    // wants_websocket: Response::websocket is public, and a plain GET route
    // may return one.
    [[nodiscard]] inline int upgrade(Request& request, Session* session, h2o_multithread_receiver_t* const hop) noexcept {
        h2o_req_t* const req = request.raw();
        if (!wants_websocket(request)) {
            delete session;
            return 400;
        }
        if (request.header("Sec-WebSocket-Version") != "13") {
            delete session;
            // RFC 6455 4.4: name the version this server speaks.
            h2o_add_header_by_str(&req->pool, &req->res.headers, H2O_STRLIT("sec-websocket-version"), 0, nullptr,
                                  H2O_STRLIT("13"));
            return 426;
        }
        const auto key = request.header("Sec-WebSocket-Key");
        if (!key.has_value() || !valid_key(req, *key)) {
            delete session;
            return 400;
        }

        wslay_event_context_ptr ctx = nullptr;
        if (wslay_event_context_server_init(&ctx, &wslay_callbacks, session) != 0) {
            delete session;
            return 500;
        }
        wslay_event_config_set_max_recv_msg_length(ctx, max_message_bytes);
        session->wslay = ctx;
        Handle* const handle = session->handle;
        handle->owner = std::this_thread::get_id();
        handle->hop = hop;
        handle->ops = &engine_ops;
        session->loop = req->conn->ctx->loop;
        h2o_timer_init(&session->reaper.timer, on_reap);
        session->reaper.session = session;
        h2o_timer_init(&session->kicker.timer, on_kick);
        session->kicker.session = session;

        char* const accept_key = static_cast<char*>(h2o_mem_alloc_pool(&req->pool, char, 29));
        create_accept_key(accept_key, key->data());

        req->res.status = 101;
        req->res.reason = "Switching Protocols";
        h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_UPGRADE, nullptr, H2O_STRLIT("websocket"));
        h2o_add_header_by_str(&req->pool, &req->res.headers, H2O_STRLIT("sec-websocket-accept"), 0, nullptr,
                              accept_key, std::strlen(accept_key));

        h2o_http1_upgrade(req, nullptr, 0, on_complete, session);
        return 101;
    }
}
