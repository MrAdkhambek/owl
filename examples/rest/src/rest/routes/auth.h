#pragma once

// POST /register and POST /login; main nests the router under /auth.

#include <coro/task.h>
#include <owl/owl.h>

#include "rest/app.h"
#include "rest/models.h"

namespace rest::auth {
    coro::task<owl::Response> register_user(const Db& db, owl::State<App> app, const owl::loop_scheduler& loop, owl::Json<Credentials> body);
    coro::task<owl::Response> login(const Db& db, owl::State<App> app, const owl::loop_scheduler& loop, owl::Json<Credentials> body);

    [[nodiscard]] owl::Router<App> router();
}
