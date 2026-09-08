#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sql::detail {
    // The owned table a result wraps. Cells are (offset, length) into one
    // contiguous byte buffer, with a NUL sentinel written after each cell --
    // cheap, and it keeps get() usable as a C string while the length is
    // what blobs need. Row-major: cell index = row * cols + col.
    struct table final {
        std::vector<std::string> names;
        std::vector<std::byte> bytes;
        struct slot final {
            std::uint32_t offset{};
            std::uint32_t length{};
        };
        std::vector<slot> cells;
        std::vector<bool> nulls;
        std::size_t rows = 0;
        std::size_t cols = 0;
        std::size_t affected = 0;
        bool has_affected = false;

        void add_column(const std::string_view name) {
            names.emplace_back(name);
            cols = names.size();
        }

        // Copies the cell and appends the sentinel, so one past every
        // stored cell is a readable NUL even after reallocation.
        void add_cell(const std::string_view data, const bool is_null) {
            const auto offset = static_cast<std::uint32_t>(bytes.size());
            const auto* const first = reinterpret_cast<const std::byte*>(data.data());
            bytes.insert(bytes.end(), first, first + data.size());
            bytes.push_back(std::byte{0});
            cells.push_back(slot{.offset = offset, .length = static_cast<std::uint32_t>(data.size())});
            nulls.push_back(is_null);
        }

        void end_row() {
            ++rows;
        }

        [[nodiscard]] std::string_view cell_bytes(const std::size_t r, const std::size_t c) const {
            const slot& s = cells[r * cols + c];
            return {reinterpret_cast<const char*>(bytes.data()) + s.offset, s.length};
        }
    };
}
