#pragma once

// The library's advertised entry point: every public header, nothing else.
// detail/ headers arrive transitively. Driver headers are gated on their
// build option -- including sql/sql.h with no driver enabled gives the core
// vocabulary only.

#include "sql/bind.h"
#include "sql/cell.h"
#include "sql/column.h"
#include "sql/dialect.h"
#include "sql/error.h"
#include "sql/execute.h"
#include "sql/null.h"
#include "sql/placeholders.h"
#include "sql/pool.h"
#include "sql/query.h"
#include "sql/reactor.h"
#include "sql/resume.h"
#include "sql/result.h"
#include "sql/result_traits.h"
#include "sql/row.h"
#include "sql/transaction.h"

#ifdef OWL_ENABLE_SQLITE
#include "sql/sqlite.h"
#include "sql/sqlite_handle.h"
#endif
#ifdef OWL_ENABLE_POSTGRESQL
#include "sql/psql.h"
#endif
