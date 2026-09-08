#pragma once

#include <coroutine>
#include <utility>

#include <coro/concepts/scheduler.h>

namespace sql {
    // Who resumes a coroutine that completed somewhere else. A non-owning
    // erasure of coro::postable -- carried by the source (the per-worker
    // sqlite_handle under owl), never by a thread: on_context_init runs on
    // the constructing thread, so a thread-local resumer set there would be
    // set N times on the main thread and never on a worker. A value on the
    // source has no thread to be wrong on. An empty resumer (standalone
    // use, a bare pool as the source) resumes inline on the completing
    // thread.
    class resumer final {
    public:
        resumer() = default;

        template <coro::postable P>
        explicit resumer(P* const p) noexcept
            : obj_(p),
              post_([](void* const o, const std::coroutine_handle<> h) {
                  static_cast<P*>(o)->post(h);
              }) {
        }

        void post(const std::coroutine_handle<> h) const {
            if (post_ != nullptr) {
                post_(obj_, h);
                return;
            }
            h.resume();
        }

        [[nodiscard]] explicit operator bool() const noexcept { return post_ != nullptr; }

    private:
        void* obj_{};
        void (*post_)(void*, std::coroutine_handle<>) {};
    };
}
