#include "rest/routes/posts.h"

#include <nlohmann/json.hpp>

#include "rest/error.h"

namespace rest::posts {
    coro::task<owl::Response> list(const Db& db) {
        const auto r = co_await sql::query<
            "SELECT p.id, u.username AS author, p.title, p.body, p.created_at "
            "FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.id DESC">(db);
        auto out = nlohmann::json::array();
        for (const auto row : r) out.push_back(post_json(row));
        co_return owl::Response::json(out);
    }

    coro::task<owl::Response> show(const Db& db, const owl::Path<"id", std::int64_t> id) {
        const auto r = co_await sql::query<
            "SELECT p.id, u.username AS author, p.title, p.body, p.created_at "
            "FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1">(db, id.value);
        if (r.rows() == 0) co_return fail(404, "no such post");
        co_return owl::Response::json(post_json(r[0]));
    }

    coro::task<owl::Response> create(const Db& db, const Cache& cache, const Bearer bearer, const owl::Json<NewPost> body) {
        const auto user = co_await user_of(cache, bearer);
        if (!user) co_return fail(401, "unknown token");
        if (body.value.title.empty()) co_return fail(422, "title required");
        const auto r = co_await sql::query<"INSERT INTO posts(user_id, title, body) VALUES ($1, $2, $3) RETURNING id">(
            db, *user, body.value.title, body.value.body);
        co_return owl::Response::json(nlohmann::json{{"id", r[0][0].as<std::int64_t>()}}, 201);
    }

    owl::Router<App> router() {
        return owl::Router<App>::make()
               .route<"/">(owl::get(list).post(create))
               .route<"/{id}">(owl::get(show));
    }
}
