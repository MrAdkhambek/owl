#pragma once

namespace sql {
    // The SQL NULL tag. Compares equal to a NULL column; binds as a NULL
    // parameter (as does nullptr, and std::optional without a value).
    struct null_t final {};

    inline constexpr null_t null{};
}
