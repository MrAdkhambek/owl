#pragma once

#include <memory>
#include <utility>

#include <coro/task.h>

#include "sql/dialect.h"
#include "sql/error.h"
#include "sql/pool.h"
#include "sql/resume.h"
#include "sql/sqlite.h"

namespace sql {
    // The sqlite source under owl: the process-wide pool plus THIS worker's
    // resumer. It is the only place the worker's resumer is recorded -- no
    // thread-local anywhere (on_context_init runs on the constructing
    // thread, so a TLS slot set there would be set N times on the main
    // thread and never on a worker; a value on the source has no thread to
    // be wrong on). A null handle (not wired) is what the extractor kicks
    // on.
    class sqlite_handle final {
    public:
        static constexpr sql::dialect dialect = sql::dialect::sqlite;

        sqlite_handle() = default;

        sqlite_handle(std::shared_ptr<pool<sqlite>> p, sql::resumer res) noexcept
            : pool_(std::move(p)), res_(res) {
        }

        [[nodiscard]] coro::task<std::expected<checkout<sqlite>, sql::error>> acquire() const {
            if (pool_ == nullptr) {
                co_return std::unexpected(sql::error{.message = "sql: sqlite pool is not wired"});
            }
            co_return co_await pool_->acquire(&res_);
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return pool_ != nullptr;
        }

        [[nodiscard]] const sql::resumer& resumer() const noexcept {
            return res_;
        }

    private:
        std::shared_ptr<pool<sqlite>> pool_;
        sql::resumer res_;
    };

    namespace detail {
        template <>
        struct driver_traits<sqlite_handle> {
            using type = sqlite;
        };
    }

    static_assert(source<sqlite_handle>);
    static_assert(source<pool<sqlite>>);
    static_assert(source<const pool<sqlite>>);
}
