#pragma once

// A throwaway sqlite file for one test, removed together with sqlite's
// journal side files when the fixture goes.

#include <filesystem>
#include <string>
#include <unistd.h>

namespace sql_test {
    struct temp_db final {
        std::filesystem::path path;

        explicit temp_db(const std::string& name)
            : path(std::filesystem::temp_directory_path() / ("owl_sql_" + std::to_string(::getpid()) + "_" + name + ".db")) {
            std::filesystem::remove(path);
        }

        ~temp_db() {
            std::filesystem::remove(path);
            std::filesystem::remove(path.string() + "-journal");
            std::filesystem::remove(path.string() + "-wal");
            std::filesystem::remove(path.string() + "-shm");
        }
    };
}
