#pragma once

#include <string>

namespace sql {
    // Every SQL failure is one of these, wrapped in std::expected -- SQL
    // failure is data. sqlstate is its own field rather than prose inside
    // message: detecting a unique violation ("23505") is the second thing
    // anyone does with a SQL error, and substring-matching it out of a
    // sentence is not an API. code is sqlite3_errcode; 0 for postgres.
    struct error final {
        std::string message;
        std::string sqlstate;
        int code{};
    };
}
