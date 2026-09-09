#pragma once

// GET /, POST / (bearer token), GET /{id}; main nests the router under
// /posts.

#include <cstdint>

#include <coro/task.h>
#include <owl/owl.h>

#include "rest/app.h"
#include "rest/auth.h"
#include "rest/models.h"

namespace rest::posts {
    coro::task<owl::Response> list(const Db& db);
    coro::task<owl::Response> show(const Db& db, owl::Path<"id", std::int64_t> id);
    coro::task<owl::Response> create(const Db& db, Bearer bearer, owl::Json<NewPost> body);

    [[nodiscard]] owl::Router<App> router();
}
