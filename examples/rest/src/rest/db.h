#pragma once

// The schema, applied once at startup.

#include <sql/psql/psql.h>

namespace rest {
    void migrate(const sql::psql::config& cfg);
}
