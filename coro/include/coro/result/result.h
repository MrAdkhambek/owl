#pragma once

// Failure as data: the error half of the toolkit. result<T> pairs a
// success value with the exception that would otherwise have been
// thrown, so a task can report failure instead of propagating it --
// as_result.h and as_tuple.h are the adapters that produce one.

#include <exception>
#include <expected>
#include <type_traits>

namespace coro {
    // std::expected<T, std::exception_ptr>, with one restriction: T may
    // not be an lvalue reference, which std::expected cannot hold anyway.
    // The constraint is stated here so it surfaces as a clean error at
    // the point of use rather than deep inside a combinator.
    template <typename T> requires (!std::is_lvalue_reference_v<T>)
    using result = std::expected<T, std::exception_ptr>;
}
