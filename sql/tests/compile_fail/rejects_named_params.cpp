#include <sql/placeholders.h>

int main() {
    constexpr auto n = sql::placeholder_count<"SELECT * FROM t WHERE k = :name">;
    return static_cast<int>(n);
}
