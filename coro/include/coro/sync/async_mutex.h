#pragma once

// A mutex whose acquisition is an awaitable. Uncontended, lock() never
// suspends -- try_lock runs first. Contended, the coroutine parks on an
// intrusive waiter list threaded through its own lock_operation
// objects: no queue allocation, no OS primitives, and waiters acquire
// in FIFO order when unlock() resumes the next one *inline* (the woken
// coroutine runs while holding the lock, on the unlocking thread).
//
// Not reentrant -- there is no owner tracking -- and there is no timed
// acquire. Neither copyable nor movable: waiter nodes point back into
// the mutex.

#include <atomic>
#include <cassert>
#include <coroutine>
#include <utility>

namespace coro {
    class async_mutex;

    namespace detail {
        // The unlock() that is currently handing a mutex down its waiter
        // queue on this thread, if any. A waiter resumed inline by that
        // unlock() usually releases the mutex before it suspends again;
        // its own unlock() must not resume the next waiter from there --
        // that would nest one unlock/resume/body triple per queued
        // waiter, and a pool worker's stack holds only a few thousand of
        // those. It sets handoff_pending instead, and the outer loop
        // resumes the next waiter after the nested one returns.
        //
        // Thread-local, not a member: the resumed waiter may keep the
        // lock, suspend, and unlock later from another thread while this
        // loop is still unwinding. State inside the mutex would then be
        // read from both threads at once; state on this thread's stack,
        // reached only through this thread's pointer, cannot be.
        struct mutex_drain {
            const async_mutex* mutex;
            bool handoff_pending;
        };

        inline mutex_drain*& active_mutex_drain() noexcept {
            thread_local mutex_drain* active = nullptr;
            return active;
        }
    }

    // Tag for building an async_mutex_lock around a mutex that is
    // already held -- the guard adopts rather than acquires.
    struct adopt_lock_t {
        explicit adopt_lock_t() = default;
    };

    inline constexpr adopt_lock_t adopt_lock{};

    // RAII guard over one held async_mutex. Destruction unlocks;
    // moving transfers the obligation (move-assignment first releases
    // any lock this guard still holds).
    class async_mutex_lock {
    public:
        explicit async_mutex_lock(async_mutex& m, adopt_lock_t) noexcept;

        async_mutex_lock(async_mutex_lock&& o) noexcept;
        async_mutex_lock& operator=(async_mutex_lock&& o) noexcept;

        async_mutex_lock(const async_mutex_lock&) = delete;
        async_mutex_lock& operator=(const async_mutex_lock&) = delete;

        ~async_mutex_lock() noexcept;

        [[nodiscard]] bool owns_lock() const noexcept {
            return mutex_ != nullptr;
        }

        explicit operator bool() const noexcept {
            return owns_lock();
        }

    private:
        async_mutex* mutex_;
    };

    class async_mutex {
    public:
        // One pending acquisition. Doubles as the awaiter for lock()
        // and as a node on the mutex's waiter lists -- hence
        // non-copyable and non-movable while parked.
        class lock_operation {
        public:
            explicit lock_operation(async_mutex& m) noexcept : mutex_(&m) {
            }

            lock_operation(const lock_operation&) = delete;
            lock_operation& operator=(const lock_operation&) = delete;
            lock_operation(lock_operation&&) = delete;
            lock_operation& operator=(lock_operation&&) = delete;

            // The fast path: if the mutex is free right now, the
            // awaiting coroutine never suspends.
            bool await_ready() const noexcept {
                return mutex_->try_lock();
            }

            // The slow path, as a race against unlock(). Two outcomes:
            //
            //   * state_ is not_locked (the mutex freed between the
            //     failed try_lock and this call): take it with a CAS
            //     and return false -- the coroutine resumes inline
            //     without ever parking.
            //   * otherwise push this node onto the waiter stack
            //     encoded in state_ and return true, leaving the
            //     coroutine parked until some unlock() resumes it.
            //
            // The push is release-paired with unlock's acquire, so the
            // unlocking thread's critical section is visible to the
            // waiter it wakes.
            bool await_suspend(const std::coroutine_handle<> continuation) noexcept {
                continuation_ = continuation;

                void* old = mutex_->state_.load(std::memory_order_acquire);
                for (;;) {
                    if (old == mutex_->not_locked()) {
                        if (mutex_->state_.compare_exchange_weak(
                            old,
                            nullptr,
                            std::memory_order_acquire,
                            std::memory_order_relaxed)) {
                            return false;
                        }
                    } else {
                        next_ = static_cast<lock_operation*>(old);
                        if (mutex_->state_.compare_exchange_weak(
                            old,
                            this,
                            std::memory_order_release,
                            std::memory_order_relaxed)) {
                            return true;
                        }
                    }
                }
            }

            void await_resume() const noexcept {
            }

        protected:
            friend class async_mutex;

            async_mutex* mutex_;
            std::coroutine_handle<> continuation_{};

            // Intrusive link to the waiter pushed before this one.
            lock_operation* next_{nullptr};
        };

        // The scoped_lock() flavour: identical acquisition, but
        // resuming hands back a guard that adopts the lock it just
        // won, so the release happens when the guard dies.
        class scoped_lock_operation : public lock_operation {
        public:
            using lock_operation::lock_operation;

            async_mutex_lock await_resume() const noexcept;
        };

        async_mutex() noexcept = default;

        async_mutex(const async_mutex&) = delete;
        async_mutex& operator=(const async_mutex&) = delete;
        async_mutex(async_mutex&&) = delete;
        async_mutex& operator=(async_mutex&&) = delete;

        // Coroutines parked on a destroyed mutex have no way home, so
        // that state is treated as a bug, not a cleanup step.
        ~async_mutex() {
            assert(
                state_.load(std::memory_order_relaxed) == not_locked() &&
                "async_mutex destroyed while still held; anything parked on it is stranded");
        }

        // Takes the lock if it is free; never blocks, never queues.
        bool try_lock() noexcept {
            void* expected = not_locked();
            return state_.compare_exchange_strong(
                expected,
                nullptr,
                std::memory_order_acquire,
                std::memory_order_relaxed
            );
        }

        // Awaitable acquisition: co_await m.lock().
        lock_operation lock() noexcept {
            return lock_operation{*this};
        }

        // The same, resuming into an async_mutex_lock guard.
        scoped_lock_operation scoped_lock() noexcept {
            return scoped_lock_operation{*this};
        }

        // Releases the mutex. If no one is waiting, it becomes free
        // with one CAS. Otherwise the next waiter is resumed *inline*,
        // on this thread, and owns the lock from the moment it wakes --
        // which is why waiters acquire in FIFO order and why state_
        // stays "held" across the handoff.
        //
        // The handoffs run as a loop here, however many waiters release
        // in turn: a waiter that unlocks while this loop is resuming it
        // is a nested call (see mutex_drain) and defers to this loop
        // rather than resuming the next waiter itself. The loop ends
        // when a resumed waiter is still holding the lock on return --
        // it suspended with it -- because from then on that waiter's own
        // unlock(), wherever it runs, is the one that continues the
        // queue.
        void unlock() noexcept {
            assert(state_.load(std::memory_order_relaxed) != not_locked() && "async_mutex::unlock() on a mutex that is not held");

            detail::mutex_drain*& active = detail::active_mutex_drain();
            if (active != nullptr && active->mutex == this) {
                active->handoff_pending = true;
                return;
            }

            // `enclosing` is a drain of some OTHER mutex whose waiter
            // this call is running inside; it is restored on the way out
            // so that waiter's own nested unlock still finds it.
            detail::mutex_drain* const enclosing = active;
            detail::mutex_drain drain{.mutex = this, .handoff_pending = false};
            active = &drain;
            for (;;) {
                lock_operation* const next = next_owner();
                if (next == nullptr) break;
                drain.handoff_pending = false;
                next->continuation_.resume();
                if (!drain.handoff_pending) break;
            }
            active = enclosing;
        }

    private:
        friend class lock_operation;

        // Pops the next owner off the FIFO, first absorbing any waiters
        // that arrived on state_ since the FIFO was last filled. Returns
        // nullptr when nobody was waiting, having released the mutex
        // outright.
        lock_operation* next_owner() noexcept {
            lock_operation* head = waiters_;
            if (head == nullptr) {
                void* expected = nullptr;
                if (state_.compare_exchange_strong(
                    expected,
                    not_locked(),
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                    return nullptr;
                }

                // A waiter landed on state_ in that window: take the
                // whole stack (acquire -- see await_suspend) and unwind
                // it from LIFO arrival order into waiters_' FIFO order.
                void* const stack = state_.exchange(nullptr, std::memory_order_acquire);
                assert(stack != nullptr && stack != not_locked());

                auto* node = static_cast<lock_operation*>(stack);
                do {
                    auto* const next = node->next_;
                    node->next_ = head;
                    head = node;
                    node = next;
                } while (node != nullptr);
            }

            waiters_ = head->next_;
            return head;
        }

        // The "free" sentinel is this mutex's own address: valid, and
        // distinct from every pointer a waiter node could occupy.
        void* not_locked() const noexcept {
            return const_cast<async_mutex*>(this);
        }

        // Lock state, encoded:
        //   not_locked()  -- free
        //   nullptr       -- held, no waiters pending on this word
        //   other         -- held; waiters are queued as a LIFO stack
        //                    of lock_operation nodes rooted here
        std::atomic<void*> state_{this};

        // Parked waiters, oldest first; empty until unlock() has had
        // to absorb a waiter stack. The head is the next owner.
        lock_operation* waiters_{nullptr};
    };

    inline async_mutex_lock::async_mutex_lock(async_mutex& m, adopt_lock_t) noexcept : mutex_(&m) {
    }

    inline async_mutex_lock::async_mutex_lock(async_mutex_lock&& o) noexcept
        : mutex_(std::exchange(o.mutex_, nullptr)) {
    }

    inline async_mutex_lock& async_mutex_lock::operator=(async_mutex_lock&& o) noexcept {
        if (this == &o) return *this;
        if (mutex_) mutex_->unlock();
        mutex_ = std::exchange(o.mutex_, nullptr);
        return *this;
    }

    inline async_mutex_lock::~async_mutex_lock() noexcept {
        if (mutex_) mutex_->unlock();
    }

    // The wait is over and the lock is held -- the guard just adopts it.
    inline async_mutex_lock async_mutex::scoped_lock_operation::await_resume() const noexcept {
        return async_mutex_lock{*mutex_, adopt_lock};
    }
}
