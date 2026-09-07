SQL

PLAN

```c++
namespace sql {
    class sqlite {
        class config {
            std::string path;
        };
        
    };
}
```

```c++
namespace sql {
    class psql {
        class config {
            std::string dns;
        };
        
    };
}
```

```c++
const auto pool = sql::pool<sql::sqlite>(sql::sqlite::config {});
const auto pool = sql::pool<sql::psql>(sql::psql::config {});

const auto std::vector<sql::row> = co_await sql::query<"QUERY">(pool);
const auto std::vector<sql::row> = co_await sql::query<"SELECT * FROM users where id = $1">(pool, [user_id]); // [user_id] is tupple


const auto coro::generator<listener_struct> = co_await sql::listen<"channel">(pool);

const auto std::vector<sql::row> = co_await sql::transaction(pool, [](const auto tx) {
    co_return co_await sql::query<"QUERY">(tx);
});
```

```c++

// read /Users/professor/CLionProjects/reflection
struct User {
    std::int64_t id{};
    std::string name;
    std::optional<std::string> user_name{};
    bool active{true};
};

template <>
struct orm::Schema<User> {
    static constexpr std::string_view table = "users";
    static constexpr Column columns[] = {
        ORM_COLUMN(User, id, Constraint::PrimaryKey | Constraint::Identity),
        ORM_COLUMN_VARCHAR(User, name, 64),
        ORM_COLUMN_VARCHAR(User, user_name, 64),
        ORM_COLUMN(User, active),
    };
};

const auto pool = sql::pool<sql::sqlite>(sql::sqlite::config {});
const auto pool = sql::pool<sql::psql>(sql::psql::config {});

const auto user_store = orm::Store<User> users(pool);

const auto std::vector<User> = co_await users.all();
const auto std::optional<User> = co_await users.find(user_id);

const auto std::vector<User> = co_await sql::query<User, "QUERY">(pool);
const auto std::vector<User> = co_await sql::query<User, "SELECT * FROM users where id = $1">(pool, [user_id]); // [user_id] is tupple

});
```