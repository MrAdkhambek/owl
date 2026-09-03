#pragma once

#include <memory>
#include <stdexcept>

namespace owl {
    // Borrows the router state owned by the Request it came from; it never
    // outlives that request, so extraction costs no refcount traffic and only
    // FromContext<State<T>> pays for a share.
    struct Context final {
        const std::shared_ptr<void>* state;
    };
}

namespace owl {
    template <typename T>
    class State final {
    public:
        State() = default;

        explicit State(std::shared_ptr<T> p) noexcept : ptr_(std::move(p)) {
        }

        T& get() const {
            if (!ptr_) throw std::runtime_error("owl::State: empty");
            return *ptr_;
        }

        T& operator*() const { return get(); }
        T* operator->() const { return &get(); }

        explicit operator bool() const noexcept { return static_cast<bool>(ptr_); }

        [[nodiscard]] std::shared_ptr<T> shared() const noexcept { return ptr_; }
        [[nodiscard]] T* get_if() const noexcept { return ptr_.get(); }

    private:
        std::shared_ptr<T> ptr_;
    };
}
