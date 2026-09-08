#pragma once

#include <chrono>
#include <concepts>
#include <memory>
#include <utility>

#include <coro/concepts/io_reactor.h>
#include <coro/io/reactor.h>
#include <coro/task.h>

namespace sql {
    // The reactor contract, erased. The psql driver only knows this shape:
    // under owl it wraps owl::loop_reactor (h2o, the worker's loop);
    // standalone it default-constructs to an owned coro::native_reactor.
    // release(fd) is the hook the psql driver calls before PQfinish -- only
    // reactors that own the fd's registration have a notion of it
    // (loop_reactor exports the h2o socket so the fd survives PQfinish);
    // for the rest it is a no-op, detected here by requires-expression, no
    // base class.
    class any_reactor final {
    public:
        any_reactor() : owned_(std::make_unique<coro::native_reactor>()) {
            wire(*owned_);
        }

        template <coro::io_reactor R>
        explicit any_reactor(R* const r) noexcept {
            wire(*r);
        }

        any_reactor(any_reactor&& o) noexcept
            : obj_(o.obj_),
              wait_(o.wait_),
              sleep_(o.sleep_),
              release_(o.release_),
              owned_(std::move(o.owned_)) {
            // A moved owned reactor lives at a new address; re-point at it.
            if (owned_ != nullptr) obj_ = owned_.get();
        }

        any_reactor& operator=(any_reactor&&) = delete;
        any_reactor(const any_reactor&) = delete;
        any_reactor& operator=(const any_reactor&) = delete;

        [[nodiscard]] coro::task<coro::wait_status>
        wait(const int fd, const coro::interest want, const std::chrono::milliseconds timeout) const {
            co_return co_await wait_(obj_, fd, want, timeout);
        }

        [[nodiscard]] coro::task<coro::wait_status> sleep(const std::chrono::milliseconds timeout) const {
            co_return co_await sleep_(obj_, timeout);
        }

        void release(const int fd) const {
            release_(obj_, fd);
        }

    private:
        template <coro::io_reactor R>
        void wire(R& r) {
            obj_ = &r;
            wait_ = [](void* const o, const int fd, const coro::interest want,
                       const std::chrono::milliseconds t) -> coro::task<coro::wait_status> {
                return static_cast<R*>(o)->wait(fd, want, t);
            };
            sleep_ = [](void* const o, const std::chrono::milliseconds t) -> coro::task<coro::wait_status> {
                return static_cast<R*>(o)->sleep(t);
            };
            if constexpr (requires(R& rr, const int fd) { rr.release(fd); }) {
                release_ = [](void* const o, const int fd) {
                    static_cast<R*>(o)->release(fd);
                };
            }
        }

        void* obj_{};
        coro::task<coro::wait_status> (*wait_)(void*, int, coro::interest, std::chrono::milliseconds){};
        coro::task<coro::wait_status> (*sleep_)(void*, std::chrono::milliseconds){};
        void (*release_)(void*, int) = [](void*, int) {};
        std::unique_ptr<coro::native_reactor> owned_;
    };

    static_assert(coro::io_reactor<any_reactor>);
}
