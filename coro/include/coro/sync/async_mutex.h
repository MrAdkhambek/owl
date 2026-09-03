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
    // Tag for building an async_mutex_lock around a mutex that is
    // already held -- the guard adopts rather than acquires.
    struct adopt_lock_t {
        explicit adopt_lock_t() = default;
    };

    inline constexpr adopt_lock_t adopt_lock{};

    class async_mutex;

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
        void unlock() noexcept {
            assert(state_.load(std::memory_order_relaxed) != not_locked() && "async_mutex::unlock() on a mutex that is not held");

            lock_operation* head = waiters_;
            if (head == nullptr) {
                void* expected = nullptr;
                if (state_.compare_exchange_strong(
                    expected,
                    not_locked(),
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                    return;
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
            head->continuation_.resume();
        }

    private:
        friend class lock_operation;

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
