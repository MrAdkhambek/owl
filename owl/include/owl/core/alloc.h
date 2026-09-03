#pragma once

#include <cstddef>
#include <type_traits>

#include <h2o.h>

namespace owl {
    template <class T>
    struct RequestAllocator final {
        using value_type = T;
        using propagate_on_container_move_assignment = std::true_type;
        using propagate_on_container_swap = std::true_type;
        using is_always_equal = std::false_type;

        h2o_mem_pool_t* pool{};

        explicit RequestAllocator(h2o_req_t* const req) noexcept : pool(&req->pool) {
        }

        explicit RequestAllocator(h2o_mem_pool_t* const p) noexcept : pool(p) {
        }

        template <class U>
        RequestAllocator(const RequestAllocator<U>& o) noexcept : pool(o.pool) {
        }

        T* allocate(const std::size_t n) {
            return static_cast<T*>(h2o_mem_alloc_pool_aligned(pool, alignof(T), n * sizeof(T)));
        }

        void deallocate(T*, std::size_t) noexcept {
        }
    };

    template <class T, class U>
    bool operator==(const RequestAllocator<T>& a, const RequestAllocator<U>& b) noexcept {
        return a.pool == b.pool;
    }
}
