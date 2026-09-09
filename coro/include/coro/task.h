#pragma once

#include <concepts>
#include <coroutine>
#include <exception>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace coro {
    namespace detail {
        // Holds the outcome of a coroutine body: a value, an exception, or
        // neither. That third state is real -- a task can be destroyed before it
        // ever runs -- and it is why this is not std::expected, which has no way
        // to spell "no result yet" and cannot store a reference at all.
        template <typename T>
        class result_slot {
            // std::variant cannot hold a reference, so T& lives here as a pointer
            // and becomes a reference again on the way out.
            using stored = std::conditional_t<std::is_reference_v<T>, std::remove_reference_t<T>*, T>;

            // Alternatives are addressed by index rather than by type: T is
            // allowed to be std::exception_ptr itself, which would make any
            // type-based lookup ambiguous.
            std::variant<std::monostate, stored, std::exception_ptr> slot_;

        public:
            // Records the value the body produced. With a reference T, from must
            // outlive the task: a conversion that materializes a temporary would
            // leave the stored pointer dangling.
            template <typename From>
            void set_value(From&& from) {
                if constexpr (std::is_reference_v<T>) slot_.template emplace<1>(std::addressof(from));
                else slot_.template emplace<1>(std::forward<From>(from));
            }

            // Records the exception that escaped the body.
            void set_exception(std::exception_ptr e) noexcept {
                slot_.template emplace<2>(e);
            }

            // Moves the result out, or rethrows the stored exception. Called from
            // the awaiting coroutine, which is the point: the throw lands on a
            // stack that can actually unwind, not inside the coroutine machinery.
            T take() {
                if (auto* e = std::get_if<2>(&slot_)) std::rethrow_exception(*e);
                if (slot_.index() != 1) throw std::logic_error("task finished without producing a result");
                if constexpr (std::is_reference_v<T>) return static_cast<T>(*std::get<1>(slot_));
                else return std::move(std::get<1>(slot_));
            }
        };

        // Same job for a body that produces no value, where an exception is the
        // only outcome worth remembering.
        template <>
        class result_slot<void> {
            std::exception_ptr e_;

        public:
            // Records the exception that escaped the body.
            void set_exception(std::exception_ptr e) noexcept {
                e_ = e;
            }

            // Rethrows that exception, if there was one.
            void take() const {
                if (e_) std::rethrow_exception(e_);
            }
        };

        // Supplies the co_return half of the promise interface. It is separate
        // from promise_type because it is the only part that differs between a
        // task that produces a value and one that does not.
        template <typename T>
        struct returns : result_slot<T> {
            // Called by the compiler for `co_return expr;`.
            //
            // When T is a reference, the argument has to be an lvalue. A
            // prvalue would satisfy plain convertibility -- `co_return 42;`
            // into a task<const int&> binds the literal to a temporary -- and
            // set_value would then store the address of something that dies
            // at the end of the co_return statement. Rejecting it here turns
            // a silent dangling read into a compile error.
            template <typename From> requires std::convertible_to<From&&, T> &&
                (!std::is_lvalue_reference_v<T> || std::is_lvalue_reference_v<From&&>)
            void return_value(From&& from) {
                this->set_value(std::forward<From>(from));
            }

            // Called by the compiler for `co_return {args...};`. A braced-init-list
            // has no type, so the deduced overload above can never match one, and a
            // coroutine would otherwise be unable to spell what a plain `return
            // {args...};` already allows. Only a value T can be built this way: for
            // a reference T the list would materialize a temporary that dies at the
            // end of the co_return statement, which is the dangling read the guard
            // above exists to reject.
            void return_value(T&& from) requires (!std::is_reference_v<T>) {
                this->set_value(std::move(from));
            }
        };

        // The void case, where there is nothing to record.
        template <>
        struct returns<void> : result_slot<void> {
            // Called by the compiler for `co_return;` or for running off the end.
            void return_void() const noexcept {
            }
        };

    }

    // A coroutine that runs only when awaited, delivers its result to whoever
    // awaited it, and can be awaited exactly once.
    //
    // Marked nodiscard because the type is lazy: dropping the returned task means
    // the body never executes at all, which is never what the caller wanted.
    template <typename T = void>
    class [[nodiscard]] task {
        // An rvalue-reference result would store the address of whatever was
        // co_returned, exactly as T& does -- but without T&'s guarantee that
        // the argument named something outliving the co_return statement, so
        // `co_return 42;` would compile and then dangle. Anything whose
        // referent does outlive the task can say so as task<T&>.
        static_assert(!std::is_rvalue_reference_v<T>, "task<T&&> is not supported: use task<T&> for a reference result");

    public:
        // The compiler-facing half of the type. It decides when the body
        // suspends, where the result goes, and who runs next when it ends.
        struct promise_type : detail::returns<T> {
            // Who to resume once this body finishes. noop_coroutine is the
            // identity element here -- it means "nobody is waiting" -- so the
            // final transfer below needs no null check and no special case.
            std::coroutine_handle<> continuation_{std::noop_coroutine()};

            // Builds the task object handed back to whoever called the coroutine.
            task get_return_object() noexcept {
                return task{std::coroutine_handle<promise_type>::from_promise(*this)};
            }

            // Suspends before the first statement, which is what makes the task
            // lazy. Starting on call would give the caller no window to attach a
            // continuation or a cancellation scope first.
            //
            // This hook and the ones below ignore `this` and so look like they
            // want to be static. They must not be: the compiler invokes them
            // through the promise object, so static members would make
            // clang-tidy's readability-static-accessed-through-instance fire at
            // every co_await and co_return written against this header, rather
            // than here. const is the right amount of tightening.
            std::suspend_always initial_suspend() const noexcept {
                return {};
            }

            // Parks an escaping exception in the result slot instead of letting
            // it tear through the coroutine machinery. take() rethrows it later.
            void unhandled_exception() noexcept {
                this->set_exception(std::current_exception());
            }

            // Runs when the body ends. Instead of returning to whoever called
            // resume(), it hands control straight to the waiting coroutine, so a
            // chain of N nested awaits finishes in constant stack space.
            struct final_awaiter {
                // Always suspends: the frame has to stay alive until its owner
                // destroys it, and the transfer below still needs to happen.
                bool await_ready() const noexcept {
                    return false;
                }

                // Names the coroutine to run next, which the compiler resumes as
                // a tail call rather than by growing the stack.
                std::coroutine_handle<>
                await_suspend(std::coroutine_handle<promise_type> me) const noexcept {
                    return me.promise().continuation_;
                }

                // Unreachable: nothing ever resumes a frame past final suspend.
                void await_resume() const noexcept {
                }
            };

            // Hands the compiler the awaiter described above.
            final_awaiter final_suspend() const noexcept {
                return {};
            }
        };

        // The typed handle to a frame of this task's coroutine.
        using handle_type = std::coroutine_handle<promise_type>;
        using value_type = T;

        // Creates an empty task that owns no frame.
        task() noexcept = default;

        // Takes over o's frame and leaves o empty.
        task(task&& o) noexcept : h_(std::exchange(o.h_, {})) {
        }

        // Destroys any frame this task already owns, then takes over o's.
        task& operator=(task&& o) noexcept {
            if (this != &o) {
                if (h_) h_.destroy();
                h_ = std::exchange(o.h_, {});
            }
            return *this;
        }

        // Copying is forbidden: two owners for one frame is a double free.
        task(const task&) = delete;
        task& operator=(const task&) = delete;

        // Destroys the frame if this task still owns one.
        ~task() {
            if (h_) h_.destroy();
        }

        // True once the body has run to completion, and true for an empty task.
        [[nodiscard]] bool done() const noexcept {
            return !h_ || h_.done();
        }

        // Runs the body up to its first suspension and leaves it there.
        void start() & {
            if (h_ && !h_.done()) h_.resume();
        }

        // The value, once done() reports the task has finished.
        T result() & {
            if (!h_) throw std::logic_error("result() on an empty task");
            return h_.promise().take();
        }

        // Drives the task to completion on the current thread. Only valid when
        // nothing in the chain suspends on external work: one resume() is all
        // the driving get() can do, so a body that parks on a timer, a pool
        // or a reactor is still parked when resume() returns. That is
        // reported as what it is -- take() would otherwise call it "finished
        // without a result" for a value task and say nothing at all for a
        // void one, and either way this task's destructor would then free a
        // frame the executor still holds.
        T get() && {
            if (!h_) throw std::logic_error("get() on an empty task");
            if (!h_.done()) h_.resume();
            if (!h_.done()) {
                throw std::logic_error("get() on a task that suspended on external work; drive it with sync_wait or an executor instead");
            }
            return h_.promise().take();
        }

        // True while this task still owns a frame.
        explicit operator bool() const noexcept {
            return static_cast<bool>(h_);
        }

        // Starts the coroutine and suspends the caller until it finishes.
        //
        // Awaiting is spelled `co_await std::move(t)` and genuinely consumes the
        // task: the awaiter takes the frame, so t is left empty and a second
        // await throws instead of resuming a finished coroutine. It also means the frame is released when the co_await
        // expression ends, rather than whenever the variable leaves scope.
        auto operator co_await() && {
            struct awaiter {
                // Owns the frame for the duration of the await.
                task t_;

                // Never ready: the task has not been started yet, so there is
                // always something to transfer control to.
                bool await_ready() const noexcept {
                    return false;
                }

                // Records the caller as the continuation, then switches to the
                // task's frame directly instead of nesting a resume() call.
                std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) const noexcept {
                    t_.h_.promise().continuation_ = awaiting;
                    return t_.h_;
                }

                // Delivers the finished task's value, or rethrows what it threw.
                T await_resume() const {
                    return t_.h_.promise().take();
                }
            };

            if (!h_) throw std::logic_error("co_await on an empty task (already awaited, or moved from)");
            return awaiter{std::move(*this)};
        }

    private:
        // Adopts a fresh frame. Only get_return_object has one to hand over.
        explicit task(handle_type h) noexcept : h_(h) {
        }

        // Null exactly when this task is empty.
        handle_type h_{};
    };

    // Whether a type is a task, asked BY NAME rather than by probing for a
    // member value_type or by testing whether some unwrapping alias changed
    // the type. A type that merely carries a value_type -- std::vector,
    // coro::result -- is not a task, and a second unwrapping specialisation
    // added later cannot answer yes here by accident.
    template <typename T>
    inline constexpr bool is_task_v = false;

    template <typename T>
    inline constexpr bool is_task_v<task<T>> = true;

    template <typename T>
    concept is_task = is_task_v<std::remove_cvref_t<T>>;

    namespace detail {
        // The value a task carries, and nothing else. The primary template is
        // deliberately EMPTY, which is what makes task_value_t below
        // SFINAE-friendly: a non-task substitutes out quietly instead of
        // hard-erroring, so `requires { typename task_value_t<T>; }` is itself
        // a usable spelling of "is this a task".
        //
        // Distinct from unwrapped in util/fmap.h, which answers a different
        // question -- "what does this contribute downstream" -- and therefore
        // falls through to identity for a non-task. Neither can be written in
        // terms of the other without losing the property that matters to it.
        template <typename T>
        struct task_value {
        };

        template <typename T>
        struct task_value<task<T>> {
            using type = T;
        };
    } // namespace detail

    // The value type a task delivers: int for task<int>, void for task<>.
    // Substitution fails for anything that is not a task.
    //
    // Generic callers spell `auto` and never need this; it is for the helper
    // that has to NAME the type:
    //
    //     using rows_t = coro::task_value_t<decltype(cx.query("SELECT 1"))>;
    template <typename T>
    using task_value_t = typename detail::task_value<std::remove_cvref_t<T>>::type;
}
