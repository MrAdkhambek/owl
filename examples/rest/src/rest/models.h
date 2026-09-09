#pragma once

// Request bodies and the one response shape. A missing or mistyped field
// fails the JSON conversion, which owl::Json<> answers with 422 before a
// handler runs.

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>
#include <sql/sql.h>

namespace rest {
    struct Credentials {
        std::string username;
        std::string password;
    };
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Credentials, username, password)

    struct NewPost {
        std::string title;
        std::string body;
    };
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(NewPost, title, body)

    // One row of the posts query -- id, author, title, body, created_at --
    // as the API shows it. A timestamptz arrives as the text postgres
    // renders it, which is what the API shows.
    inline nlohmann::json post_json(const sql::psql::row row) {
        return {
            {"id", row["id"].as<std::int64_t>()},
            {"author", row["author"].as<std::string>()},
            {"title", row["title"].as<std::string>()},
            {"body", row["body"].as<std::string>()},
            {"created_at", row["created_at"].as<std::string>()},
        };
    }
}
