# The sql drivers as a header-only library, layered like coro: public headers
# under include/sql/, consumed as "sql/..." through four dependency tiers so
# a driver's headers and C-library link never exist when its option is off:
#
#     target_link_libraries(my_target PRIVATE owl::sql)
#     #include "sql/sql.h"            -- drivers gated by OWL_ENABLE_*
#
# OWL_ENABLE_SQLITE / OWL_ENABLE_POSTGRESQL are compile definitions on the
# driver targets (not here): every TU that must see the macro links a driver
# target and inherits it.

set(_sql_hdrs
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/bind.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/cell.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/column.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/dialect.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/error.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/execute.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/null.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/placeholders.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/pool.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/query.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/reactor.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/resume.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/result.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/result_traits.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/row.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/sql.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/transaction.h"
)

add_library(sql_core INTERFACE)
add_library(${PROJECT_NAME}::sql_core ALIAS sql_core)

target_compile_features(sql_core INTERFACE cxx_std_23)

target_link_libraries(sql_core INTERFACE coro fstr)

if (OWL_ENABLE_SQLITE)
    if (APPLE AND NOT SQLite3_ROOT)
        execute_process(
                COMMAND brew --prefix sqlite
                OUTPUT_VARIABLE SQLite3_ROOT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
        )
    endif ()
    find_package(SQLite3 REQUIRED)
endif ()

if (OWL_ENABLE_POSTGRESQL)
    # brew's libpq is keg-only: its .pc is not on the default search path.
    if (APPLE)
        execute_process(
                COMMAND brew --prefix libpq
                OUTPUT_VARIABLE _sql_pq_home
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
        )
        if (_sql_pq_home)
            set(ENV{PKG_CONFIG_PATH} "${_sql_pq_home}/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
        endif ()
    endif ()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PQ REQUIRED IMPORTED_TARGET libpq)
endif ()

if (OWL_ENABLE_SQLITE)
    add_library(sql_sqlite INTERFACE)
    add_library(${PROJECT_NAME}::sql_sqlite ALIAS sql_sqlite)
    target_compile_features(sql_sqlite INTERFACE cxx_std_23)
    target_link_libraries(sql_sqlite INTERFACE sql_core SQLite::SQLite3)
    target_compile_definitions(sql_sqlite INTERFACE OWL_ENABLE_SQLITE)
    target_sources(sql_sqlite INTERFACE
            FILE_SET HEADERS
            BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
            FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/sqlite.h"
    )
endif ()

if (OWL_ENABLE_POSTGRESQL)
    add_library(sql_psql INTERFACE)
    add_library(${PROJECT_NAME}::sql_psql ALIAS sql_psql)
    target_compile_features(sql_psql INTERFACE cxx_std_23)
    target_link_libraries(sql_psql INTERFACE sql_core PkgConfig::PQ)
    target_compile_definitions(sql_psql INTERFACE OWL_ENABLE_POSTGRESQL)
    target_sources(sql_psql INTERFACE
            FILE_SET HEADERS
            BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
            FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/psql.h"
    )
endif ()

add_library(sql INTERFACE)
add_library(${PROJECT_NAME}::sql ALIAS sql)

target_compile_features(sql INTERFACE cxx_std_23)

target_include_directories(sql INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_sources(sql INTERFACE
        FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES ${_sql_hdrs}
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/detail/table.h"
)

if (OWL_ENABLE_SQLITE)
    target_link_libraries(sql INTERFACE sql_sqlite)
endif ()
if (OWL_ENABLE_POSTGRESQL)
    target_link_libraries(sql INTERFACE sql_psql)
endif ()

if (BUILD_TESTING)
    function(sql_add_test path)
        string(REPLACE "/" "_" name "${path}")
        add_executable(${name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/${path}.cpp")
        target_link_libraries(${name}
                PRIVATE
                ${PROJECT_NAME}::sql
                GTest::gtest_main)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    function(sql_add_compile_fail_test name)
        add_executable(${name} EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/${name}.cpp")
        target_link_libraries(${name} PRIVATE ${PROJECT_NAME}::sql)
        add_test(NAME ${name}
                COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}"
                        --target ${name} --config $<CONFIG>)
        set_tests_properties(${name} PROPERTIES WILL_FAIL TRUE)
    endfunction()

    sql_add_test(test_cell)
    sql_add_test(test_row)
    sql_add_test(test_placeholders)
    sql_add_test(test_binds)
    sql_add_test(test_resume_reactor)
    sql_add_test(test_sqlite_driver)
    sql_add_test(test_pool)
    sql_add_test(test_sqlite)
    sql_add_test(test_transaction)

    sql_add_compile_fail_test(rejects_arity_mismatch)
    sql_add_compile_fail_test(rejects_dialect_mismatch)

    sql_add_compile_fail_test(rejects_mixed_styles)
    sql_add_compile_fail_test(rejects_named_params)
    sql_add_compile_fail_test(rejects_numbered_params)
    sql_add_compile_fail_test(rejects_holes)

    # One TU per public header, each including only that header -- proves
    # self-containment and makes "compiled by nothing" impossible. Driver
    # headers only compile when their option is on (they #error otherwise).
    set(_sql_hdr_dir "${CMAKE_CURRENT_BINARY_DIR}/header_checks")
    set(_sql_hdr_srcs "")
    foreach (_h IN LISTS _sql_hdrs)
        string(REPLACE "/" "_" _stem "${_h}")
        string(REPLACE ".h" "" _stem "${_stem}")
        set(_src "${_sql_hdr_dir}/${_stem}.cpp")
        file(CONFIGURE OUTPUT "${_src}"
                CONTENT "// generated by sql/CMakeLists.txt -- do not edit\n#include \"${_h}\"\n")
        list(APPEND _sql_hdr_srcs "${_src}")
    endforeach ()
    add_library(sql_headers OBJECT ${_sql_hdr_srcs})
    target_link_libraries(sql_headers PRIVATE ${PROJECT_NAME}::sql_core)
    if (OWL_ENABLE_SQLITE)
        set(_src "${_sql_hdr_dir}/sql_sqlite_h.cpp")
        file(CONFIGURE OUTPUT "${_src}"
                CONTENT "// generated by sql/CMakeLists.txt -- do not edit\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/include/sql/sqlite.h\"\n")
        target_sources(sql_headers PRIVATE "${_src}")
        target_link_libraries(sql_headers PRIVATE ${PROJECT_NAME}::sql_sqlite)
    endif ()
    if (OWL_ENABLE_POSTGRESQL)
        set(_src "${_sql_hdr_dir}/sql_psql_h.cpp")
        file(CONFIGURE OUTPUT "${_src}"
                CONTENT "// generated by sql/CMakeLists.txt -- do not edit\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/include/sql/psql.h\"\n")
        target_sources(sql_headers PRIVATE "${_src}")
        target_link_libraries(sql_headers PRIVATE ${PROJECT_NAME}::sql_psql)
    endif ()
endif ()
