#pragma once

// native_reactor -- readiness waiting for file descriptors, backed by
// the platform's multiplexer: kqueue on macOS/BSD, epoll on Linux.
//
// The shape of the thing: one background thread owns the kernel queue
// and does all the blocking. A coroutine that co_awaits wait() or
// sleep() parks a small request (which lives in its own frame), the
// reactor thread arms ONESHOT kernel events for it, and when an event
// fires the parked coroutine is resumed -- inline, on the reactor
// thread -- with a wait_status. Nothing polls, nothing spins: the
// reactor thread sleeps in kevent/epoll_wait until the kernel speaks.
//
// Tear down only when nothing is parked. Destruction does resume
// stragglers with wait_status::cancelled as a last resort, but a
// coroutine resumed that way must not touch the reactor again.
//
// One waiter per descriptor per direction at a time: registrations are
// keyed by (fd, filter), so a second read wait on an fd that already
// has one replaces it in the kernel and the first never completes. A
// read wait and a write wait on the same fd are fine, and each
// completes without disturbing the other.

#include <cerrno>
#include <chrono>
#include <coroutine>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define CORO_REACTOR_KQUEUE 1
#include <sys/event.h>
#include <unistd.h>
#elif defined(__linux__)
#define CORO_REACTOR_EPOLL 1
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#else
#error "coro::native_reactor needs kqueue or epoll"
#endif

#include "coro/concepts/io_reactor.h"

namespace coro {
    class native_reactor {
        // Everything one wait() or sleep() needs to be armed, matched
        // with its event, completed, and torn down again. A pointer to
        // this struct is handed to the kernel as the event's identity
        // (kqueue udata / epoll data.ptr), so it must stay valid while
        // the request is parked -- and it does: it lives in the
        // awaiting coroutine's frame, which cannot run, and therefore
        // cannot die, before completion resumes it.
        struct request {
            std::coroutine_handle<> continuation{};
            wait_status status = wait_status::error;
            int fd = -1;
            interest want = interest::read;
            // -1 means "no deadline"; >= 0 arms a timer. sleep() uses
            // fd == -1, so a sleep is just a wait with no fd to watch.
            std::chrono::milliseconds timeout{-1};
#if CORO_REACTOR_EPOLL
            // epoll has no native timer, so each timed request carries
            // its own timerfd (created on arm, closed on disarm).
            int timer_fd = -1;
#endif
        };

        // The guts: kernel queue, reactor thread, and the bookkeeping
        // between them. Behind a unique_ptr so native_reactor itself
        // stays movable while the thread's `this` never relocates.
        struct impl {
#if CORO_REACTOR_KQUEUE
            int kq = -1;
#elif CORO_REACTOR_EPOLL
            int epfd = -1;
            // An eventfd that owns the reactor thread awake. Registered
            // with data.ptr == nullptr, which is how run() tells "wake
            // signal" apart from "real request".
            int wake_fd = -1;
#endif
            std::mutex mutex;
            // Handed to the thread on submit; the thread moves them
            // into inflight once armed.
            std::vector<request*> pending;
            std::vector<request*> cancels;
            // Armed with the kernel; the set is what makes completion
            // exactly-once.
            std::unordered_set<request*> inflight;
            std::thread thread;
            bool stopping = false;

            impl() {
#if CORO_REACTOR_KQUEUE
                kq = ::kqueue();
                if (kq < 0) throw std::system_error(errno, std::generic_category(), "kqueue");
                // A user event exists purely to break the thread's
                // kevent wait -- on submit and on shutdown.
                struct kevent ev{};
                EV_SET(&ev, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
                ::kevent(kq, &ev, 1, nullptr, 0, nullptr);
#elif CORO_REACTOR_EPOLL
                epfd = ::epoll_create1(EPOLL_CLOEXEC);
                if (epfd < 0) throw std::system_error(errno, std::generic_category(), "epoll_create1");
                wake_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
                if (wake_fd < 0) throw std::system_error(errno, std::generic_category(), "eventfd");
                epoll_event ev{};
                ev.events = EPOLLIN;
                ev.data.ptr = nullptr;
                ::epoll_ctl(epfd, EPOLL_CTL_ADD, wake_fd, &ev);
#endif
                thread = std::thread([this] {
                    run();
                });
            }

            impl(const impl&) = delete;
            impl& operator=(const impl&) = delete;

            // Signals the thread, which cancels every pending and
            // inflight request (resuming each with wait_status::cancelled)
            // before returning. Only then are the kernel fds closed.
            ~impl() {
                {
                    const std::lock_guard lock(mutex);
                    stopping = true;
                }
                wake();
                thread.join();
#if CORO_REACTOR_KQUEUE
                if (kq >= 0) ::close(kq);
#elif CORO_REACTOR_EPOLL
                if (wake_fd >= 0) ::close(wake_fd);
                if (epfd >= 0) ::close(epfd);
#endif
            }

            // Breaks the thread out of its blocking wait.
            void wake() const {
#if CORO_REACTOR_KQUEUE
                struct kevent ev{};
                EV_SET(&ev, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
                ::kevent(kq, &ev, 1, nullptr, 0, nullptr);
#elif CORO_REACTOR_EPOLL
                const std::uint64_t one = 1;
                ::write(wake_fd, &one, sizeof(one));
#endif
            }

            // Coroutine-side entry: queue the request, then kick the
            // thread so it arms the new events promptly.
            void submit(request* req) {
                {
                    const std::lock_guard lock(mutex);
                    pending.push_back(req);
                }
                wake();
            }

            // Ends whatever is waiting on a descriptor right now. The
            // requests are picked here, under the lock, and only they are
            // queued -- naming the descriptor instead would let a cancel
            // outlive the wait it was meant for and fell on whatever waited
            // on that number next. Completing them is left to the reactor
            // thread, so a waiter is resumed there like every other
            // completion whichever thread asked. A descriptor with nothing
            // waiting on it has nothing to cancel, and the call does
            // nothing at all.
            void cancel(const int fd) {
                {
                    const std::lock_guard lock(mutex);
                    for (request* const req : pending) {
                        if (req->fd == fd) cancels.push_back(req);
                    }
                    for (request* const req : inflight) {
                        if (req->fd == fd) cancels.push_back(req);
                    }
                }
                wake();
            }

            // Exactly-once completion: only the first caller for a
            // request gets past the inflight check (an fd event and its
            // timer can both fire in a race; one completion must win).
            // The losers fall through and do nothing. The winner
            // disarms the rest of the request's registrations and
            // resumes the coroutine with the status.
            void complete(request* req, const wait_status status) {
                {
                    const std::lock_guard lock(mutex);
                    if (inflight.erase(req) == 0) return;
                }
                disarm(req);
                req->status = status;
                const std::coroutine_handle<> continuation = req->continuation;
                req->continuation = {};
                continuation.resume();
            }

#if CORO_REACTOR_KQUEUE
            // Submits a changelist and reports whether every change took.
            // Each change carries EV_RECEIPT, and the eventlist has one
            // slot per change: the kernel then answers every change with
            // a receipt -- EV_ERROR set, data holding errno or zero --
            // and keeps processing past a failed one. Without the slots
            // it would stop at the first failure and report -1, leaving
            // the changes after it unapplied; that is how a failed fd
            // registration used to leave its timer unarmed, and a
            // oneshot filter that had already fired used to leave the
            // timer deleted after it still ticking.
            [[nodiscard]] bool apply(struct kevent* changes, const int n) const {
                if (n == 0) return true;
                struct kevent receipts[3]{};
                const int got = ::kevent(kq, changes, n, receipts, n, nullptr);
                if (got < 0) return false;
                bool ok = true;
                for (int i = 0; i < got; ++i) {
                    if ((receipts[i].flags & EV_ERROR) && receipts[i].data != 0) ok = false;
                }
                return ok;
            }
#endif

            // Removes what arm() registered for this request -- only
            // that: another request may hold the other direction's
            // filter on the same fd, and it must survive this. Safe to
            // call more than once, and after a oneshot has fired: a
            // missing registration is simply reported in its receipt
            // and the deletes after it still go through.
            void disarm(request* req) const {
#if CORO_REACTOR_KQUEUE
                struct kevent ev[3]{};
                int n = 0;
                if (req->fd >= 0) {
                    if (req->want != interest::write) {
                        EV_SET(&ev[n++], req->fd, EVFILT_READ, EV_DELETE | EV_RECEIPT, 0, 0, nullptr);
                    }
                    if (req->want != interest::read) {
                        EV_SET(&ev[n++], req->fd, EVFILT_WRITE, EV_DELETE | EV_RECEIPT, 0, 0, nullptr);
                    }
                }
                // The timer is keyed by the request's own address, so it
                // can be found and deleted without extra state.
                if (req->timeout.count() >= 0) {
                    EV_SET(&ev[n++], reinterpret_cast<uintptr_t>(req), EVFILT_TIMER, EV_DELETE | EV_RECEIPT, 0, 0, nullptr);
                }
                static_cast<void>(apply(ev, n));
#elif CORO_REACTOR_EPOLL
                if (req->fd >= 0) ::epoll_ctl(epfd, EPOLL_CTL_DEL, req->fd, nullptr);
                if (req->timer_fd >= 0) {
                    ::epoll_ctl(epfd, EPOLL_CTL_DEL, req->timer_fd, nullptr);
                    ::close(req->timer_fd);
                    req->timer_fd = -1;
                }
#endif
            }

            // Registers the kernel events for a fresh request. All
            // registrations are ONESHOT: they fire at most once, which
            // is what lets completion be "disarm what is left" instead
            // of reference counting.
            //
            // False when the kernel refused any of it -- a closed fd
            // (EBADF), an fd that already carries this direction on
            // epoll (EEXIST), no timerfd to be had. The caller turns
            // that into wait_status::error; ignoring it would leave the
            // request in `inflight` with nothing armed to ever complete
            // it, parked for good.
            [[nodiscard]] bool arm(request* req) const {
#if CORO_REACTOR_KQUEUE
                struct kevent ev[3]{};
                int n = 0;
                if (req->fd >= 0) {
                    if (req->want != interest::write) {
                        EV_SET(&ev[n++], req->fd, EVFILT_READ, EV_ADD | EV_ONESHOT | EV_RECEIPT, 0, 0, req);
                    }
                    if (req->want != interest::read) {
                        EV_SET(&ev[n++], req->fd, EVFILT_WRITE, EV_ADD | EV_ONESHOT | EV_RECEIPT, 0, 0, req);
                    }
                }
                if (req->timeout.count() >= 0) {
                    EV_SET(&ev[n++], reinterpret_cast<uintptr_t>(req), EVFILT_TIMER, EV_ADD | EV_ONESHOT | EV_RECEIPT, 0,
                           static_cast<intptr_t>(req->timeout.count()), req);
                }
                return apply(ev, n);
#elif CORO_REACTOR_EPOLL
                bool ok = true;
                if (req->fd >= 0) {
                    epoll_event ev{};
                    ev.events = EPOLLET | EPOLLONESHOT;
                    if (req->want != interest::write) ev.events |= EPOLLIN;
                    if (req->want != interest::read) ev.events |= EPOLLOUT;
                    ev.data.ptr = req;
                    ok = ::epoll_ctl(epfd, EPOLL_CTL_ADD, req->fd, &ev) == 0;
                }
                if (req->timeout.count() >= 0) {
                    req->timer_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
                    if (req->timer_fd < 0) return false;
                    itimerspec spec{};
                    spec.it_value.tv_sec = req->timeout.count() / 1000;
                    spec.it_value.tv_nsec = (req->timeout.count() % 1000) * 1'000'000L;
                    // timerfd rejects an all-zero expiry; the smallest
                    // legal nudge stands in for "fire now".
                    if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0) {
                        spec.it_value.tv_nsec = 1;
                    }
                    if (::timerfd_settime(req->timer_fd, 0, &spec, nullptr) != 0) return false;
                    epoll_event ev{};
                    ev.events = EPOLLIN | EPOLLONESHOT;
                    ev.data.ptr = req;
                    if (::epoll_ctl(epfd, EPOLL_CTL_ADD, req->timer_fd, &ev) != 0) return false;
                }
                return ok;
#endif
            }

            // The reactor thread. One event per iteration keeps the
            // platform-specific halves small; resuming a coroutine can
            // submit new requests, and those are picked up on the next
            // pass.
            void run() {
                for (;;) {
                    std::vector<request*> batch;
                    std::vector<request*> cancel;
                    // stopping is written by the destructor under the
                    // mutex, so it is read under the mutex too and
                    // carried out as a local; the branch below runs
                    // unlocked.
                    std::vector<request*> asked;
                    bool stop = false;
                    {
                        const std::lock_guard lock(mutex);
                        stop = stopping;
                        if (stop) {
                            cancel.swap(pending);
                            cancel.insert(cancel.end(), inflight.begin(), inflight.end());
                            inflight.clear();
                        } else {
                            batch.swap(pending);
                            for (request* req : batch) inflight.insert(req);
                            // Already chosen by cancel(); the pointers are
                            // only compared and erased below, never
                            // dereferenced before complete() has won the
                            // race for them, so one that finished in the
                            // meantime is simply not found.
                            asked.swap(cancels);
                        }
                    }
                    if (stop) {
                        for (request* req : cancel) {
                            disarm(req);
                            req->status = wait_status::cancelled;
                            req->continuation.resume();
                        }
                        return;
                    }
                    // A refused registration completes right here, on
                    // the reactor thread, the same way a fired event
                    // would: the coroutine learns of it as data.
                    for (request* req : batch) {
                        if (!arm(req)) complete(req, wait_status::error);
                    }
                    // After arming, never before: a request cancelled in
                    // the same pass that armed it must be armed first, or
                    // complete() resumes it -- freeing its frame, and with
                    // it the request -- while the arm loop still holds the
                    // pointer.
                    for (request* const req : asked) complete(req, wait_status::cancelled);

#if CORO_REACTOR_KQUEUE
                    struct kevent ev{};
                    const int n = ::kevent(kq, nullptr, 0, &ev, 1, nullptr);
                    if (n <= 0) continue;
                    if (ev.filter == EVFILT_USER) continue;
                    auto* req = static_cast<request*>(ev.udata);
                    if (req == nullptr) continue;
                    const wait_status st = ev.filter == EVFILT_TIMER
                                               ? wait_status::timeout
                                               : (ev.flags & EV_ERROR)
                                               ? wait_status::error
                                               : wait_status::ready;
                    complete(req, st);
#elif CORO_REACTOR_EPOLL
                    epoll_event ev{};
                    const int n = ::epoll_wait(epfd, &ev, 1, -1);
                    if (n <= 0) continue;
                    if (ev.data.ptr == nullptr) {
                        std::uint64_t buf{};
                        ::read(wake_fd, &buf, sizeof(buf));
                        continue;
                    }
                    auto* req = static_cast<request*>(ev.data.ptr);
                    wait_status st = wait_status::ready;
                    // A readable timerfd has expired; the read both
                    // confirms that and drains the expiry count, which
                    // the fd requires before it can signal again.
                    if (req->timer_fd >= 0 && (ev.events & EPOLLIN)) {
                        std::uint64_t exp{};
                        if (::read(req->timer_fd, &exp, sizeof(exp)) > 0) st = wait_status::timeout;
                    }
                    // Only a genuine error is an error. EPOLLHUP is a
                    // readiness condition -- the read will return EOF, the
                    // write will fail at once, neither will block -- and
                    // the kernel reports it whether asked or not, alongside
                    // EPOLLIN while data is still buffered on a half-closed
                    // socket. Calling that an error would leave those bytes
                    // unread; kqueue reports the same state as EV_EOF on a
                    // ready event, and the two backends should agree.
                    if (ev.events & EPOLLERR) st = wait_status::error;
                    complete(req, st);
#endif
                }
            }
        };

        // The awaiter that carries a request through one wait. The
        // request lives inside the awaiting coroutine's frame -- no
        // allocation -- and the coroutine stays parked until the
        // reactor thread resumes it.
        struct park {
            impl* owner = nullptr;
            request req{};

            bool await_ready() const noexcept {
                return false;
            }

            void await_suspend(const std::coroutine_handle<> continuation) {
                req.continuation = continuation;
                owner->submit(&req);
            }

            [[nodiscard]] wait_status await_resume() const noexcept {
                return req.status;
            }
        };

    public:
        // Creates the kernel queue and starts the reactor thread.
        native_reactor() : impl_(std::make_unique<impl>()) {
        }

        // Movable: the guts sit behind unique_ptr, so moving the shell
        // never invalidates the impl pointer the reactor thread (or a
        // parked request) holds.
        native_reactor(native_reactor&&) noexcept = default;
        native_reactor& operator=(native_reactor&&) noexcept = default;
        native_reactor(const native_reactor&) = delete;
        native_reactor& operator=(const native_reactor&) = delete;
        ~native_reactor() = default;

        // Parks until fd is ready for the wanted interest, the timeout
        // expires, or something errors. timeout < 0 means wait forever.
        // A negative fd reports error without parking; one the kernel
        // refuses to register (closed, say) reports error from the
        // reactor thread, after the hop.
        [[nodiscard]] task<wait_status> wait(
            const int fd, const interest want, const std::chrono::milliseconds timeout) const {
            if (fd < 0) co_return wait_status::error;
            park p{.owner = impl_.get(), .req = request{.fd = fd, .want = want, .timeout = timeout}};
            co_return co_await p;
        }

        // Ends a wait parked on this descriptor: the waiter resumes with
        // wait_status::cancelled and the descriptor itself is untouched, so
        // its owner still decides when to close it. Safe from any thread,
        // and a no-op when nothing is parked on that fd.
        void cancel(const int fd) const {
            if (fd >= 0) impl_->cancel(fd);
        }

        // Parks for the timeout, then reports wait_status::timeout. A
        // non-positive timeout returns ready immediately, without
        // touching the reactor thread.
        [[nodiscard]] task<wait_status> sleep(const std::chrono::milliseconds timeout) const {
            if (timeout.count() <= 0) co_return wait_status::ready;
            park p{.owner = impl_.get(), .req = request{.want = interest::read, .timeout = timeout}};
            co_return co_await p;
        }

    private:
        std::unique_ptr<impl> impl_;
    };

    static_assert(io_reactor<native_reactor>);
}
