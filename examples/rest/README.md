# rest

A small REST API on owl + sql -- register, log in, read and write posts --
laid out the way an axum project is.

| File | axum counterpart | Holds |
|---|---|---|
| `src/rest/main.cpp` | `main.rs` | migrations, the composed router, the server |
| `src/rest/app.h` | `state.rs` | `App`, the state every handler can take as `owl::State<App>` |
| `src/rest/db.h/.cpp` | `db.rs` | the schema, applied once at startup through a standalone pool |
| `src/rest/auth.h/.cpp` | `auth.rs` + an extractor | PBKDF2 password hashing as tasks, session tokens, the `Bearer` extractor |
| `src/rest/models.h` | `models.rs` | request bodies (`Credentials`, `NewPost`) and the post JSON |
| `src/rest/error.h` | `error.rs` | `fail(status, message)` -> `{"error": "..."}` |
| `src/rest/routes/auth.h/.cpp` | `routes/auth.rs` | `POST /register`, `POST /login`, and their `router()` |
| `src/rest/routes/posts.h/.cpp` | `routes/posts.rs` | `GET /`, `POST /`, `GET /{id}`, and their `router()` |

Each module owns a router; `main.cpp` nests them under `/auth` and `/posts`.
SQLite comes from each worker's pool (`const Db&`). Password hashing is a
task piped onto a small thread pool and back:
`co_await (hash_password(pw) | coro::schedule_on(app->hashing) | coro::resume_on(loop))`.

## Build and run

This directory is its own CMake project. It pulls owl in with
`add_subdirectory(../..)`, the way an application vendors the library
(after `cmake --install`, `find_package(owl)` gives the same targets), and
needs what owl needs: libh2o-evloop, OpenSSL, zlib, and sqlite3.

```sh
cmake -S examples/rest -B examples/rest/build
cmake --build examples/rest/build
./examples/rest/build/rest
```

It creates `rest.db` in the working directory and listens on port 8080.

## Try it

```sh
J='content-type: application/json'
curl -s -XPOST localhost:8080/auth/register -H "$J" -d '{"username":"ann","password":"hunter2hunter2"}'
TOKEN=$(curl -s -XPOST localhost:8080/auth/login -H "$J" -d '{"username":"ann","password":"hunter2hunter2"}' | sed 's/.*"token":"\([^"]*\)".*/\1/')
curl -s -XPOST localhost:8080/posts -H "authorization: Bearer $TOKEN" -H "$J" -d '{"title":"hello","body":"first post"}'
curl -s localhost:8080/posts
curl -s localhost:8080/posts/1
```

| Route | Answers |
|---|---|
| `POST /auth/register` | 201 `{"id", "username"}`; 409 if the name is taken; 422 for a bad field |
| `POST /auth/login` | 200 `{"token"}`; 401 for bad credentials |
| `GET /posts` | 200, newest first |
| `POST /posts` | needs `Authorization: Bearer <token>`; 201 `{"id"}`; 401 without or with an unknown token |
| `GET /posts/{id}` | 200 or 404 |

A body that is not the JSON asked for gets the framework's own 415 / 400 / 422.

## Postman

Import `postman/owl-rest.postman_collection.json`. Run the requests top to
bottom: **Login** stores the token in the collection variable `token`,
**Create post** stores `postId`, and each request carries a test for its
expected status.
