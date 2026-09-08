# owl::sql -- async SQL over coro, header-only, in coro's layout: public
# headers under include/sql/, consumed with their "sql/" prefix, the
# FILE_SET as the only header list, and the same tree / manifest / umbrella
# guard so the three cannot drift.
#
# The drivers are optional because each needs a C library: psql.h needs
# libpq (OWL_ENABLE_POSTGRESQL), sqlite.h needs sqlite3 (OWL_ENABLE_SQLITE).
# The macros ride on this target, so owl sees them transitively.

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
        FILES
        include/sql/sql.h
        include/sql/error.h
        include/sql/convert.h
        include/sql/io.h
        include/sql/detail/placeholders.h
        include/sql/detail/stmt_key.h
)

target_link_libraries(sql INTERFACE coro fstr)

if (OWL_ENABLE_POSTGRESQL)
    # brew's libpq is keg-only; hint it the way owl hints OpenSSL.
    if (APPLE AND NOT PostgreSQL_ROOT)
        execute_process(COMMAND brew --prefix libpq
                OUTPUT_VARIABLE PostgreSQL_ROOT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
    endif ()
    find_package(PostgreSQL REQUIRED)
    target_link_libraries(sql INTERFACE PostgreSQL::PostgreSQL)
    target_compile_definitions(sql INTERFACE OWL_ENABLE_POSTGRESQL)
endif ()

if (OWL_ENABLE_SQLITE)
    if (APPLE AND NOT SQLite3_ROOT)
        execute_process(COMMAND brew --prefix sqlite
                OUTPUT_VARIABLE SQLite3_ROOT
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
    endif ()
    find_package(SQLite3 REQUIRED)
    target_link_libraries(sql INTERFACE SQLite::SQLite3)
    target_compile_definitions(sql INTERFACE OWL_ENABLE_SQLITE)
endif ()

# --- Tree / manifest / umbrella consistency (coro's guard, verbatim) ------

file(GLOB_RECURSE _sql_on_disk
        RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/include"
        CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/*.h")
list(SORT _sql_on_disk)

get_target_property(_sql_manifest_raw sql HEADER_SET)
set(_sql_manifest "")
foreach (_f IN LISTS _sql_manifest_raw)
    cmake_path(ABSOLUTE_PATH _f
            BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE
            OUTPUT_VARIABLE _abs)
    cmake_path(RELATIVE_PATH _abs
            BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include"
            OUTPUT_VARIABLE _rel)
    list(APPEND _sql_manifest "${_rel}")
endforeach ()
list(SORT _sql_manifest)

if (NOT _sql_on_disk STREQUAL _sql_manifest)
    set(_missing ${_sql_on_disk})
    if (_sql_manifest)
        list(REMOVE_ITEM _missing ${_sql_manifest})
    endif ()
    set(_stale ${_sql_manifest})
    if (_sql_on_disk)
        list(REMOVE_ITEM _stale ${_sql_on_disk})
    endif ()
    message(FATAL_ERROR
            "sql: FILE_SET does not match the header tree.\n"
            "  on disk but not in FILE_SET (would not install): ${_missing}\n"
            "  in FILE_SET but not on disk: ${_stale}")
endif ()

set(_sql_public ${_sql_on_disk})
list(REMOVE_ITEM _sql_public "sql/sql.h")
list(FILTER _sql_public EXCLUDE REGEX "/detail/")
list(SORT _sql_public)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/sql/sql.h" _sql_umbrella_lines
        REGEX "^#include \"sql/")
set(_sql_umbrella "")
foreach (_line IN LISTS _sql_umbrella_lines)
    string(REGEX REPLACE "^#include \"([^\"]+)\".*$" "\\1" _inc "${_line}")
    list(APPEND _sql_umbrella "${_inc}")
endforeach ()
list(SORT _sql_umbrella)

if (NOT _sql_public STREQUAL _sql_umbrella)
    set(_unumbrellaed ${_sql_public})
    if (_sql_umbrella)
        list(REMOVE_ITEM _unumbrellaed ${_sql_umbrella})
    endif ()
    set(_phantom ${_sql_umbrella})
    if (_sql_public)
        list(REMOVE_ITEM _phantom ${_sql_public})
    endif ()
    message(FATAL_ERROR
            "sql: sql.h does not match the public header tree.\n"
            "  public but not in the umbrella: ${_unumbrellaed}\n"
            "  in the umbrella but not public (or a detail/ header): ${_phantom}")
endif ()

if (BUILD_TESTING)
    # One TU per public header, each including only that header, so a
    # header that forgot an include cannot hide behind the umbrella. The
    # driver headers are checked only when their C library is linked.
    set(_sql_hdr_dir "${CMAKE_CURRENT_BINARY_DIR}/header_checks")
    set(_sql_hdr_srcs "")
    foreach (_h IN LISTS _sql_public)
        if (_h STREQUAL "sql/psql.h" AND NOT OWL_ENABLE_POSTGRESQL)
            continue()
        endif ()
        if (_h STREQUAL "sql/sqlite.h" AND NOT OWL_ENABLE_SQLITE)
            continue()
        endif ()
        string(REPLACE "/" "_" _stem "${_h}")
        string(REPLACE ".h" "" _stem "${_stem}")
        set(_src "${_sql_hdr_dir}/${_stem}.cpp")
        file(CONFIGURE OUTPUT "${_src}"
                CONTENT "// generated by sql/CMakeLists.txt -- do not edit\n#include <${_h}>\n")
        list(APPEND _sql_hdr_srcs "${_src}")
    endforeach ()

    add_library(sql_headers OBJECT ${_sql_hdr_srcs})
    target_link_libraries(sql_headers PRIVATE ${PROJECT_NAME}::sql)

    # Mirrors the header tree: tests/psql/test_psql -> target sql_psql_test_psql.
    # The sql_ prefix keeps clear of coro's and owl's test_* targets.
    function(sql_add_test path)
        string(REPLACE "/" "_" name "${path}")
        set(name "sql_${name}")
        add_executable(${name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/${path}.cpp")
        target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        target_link_libraries(${name}
                PRIVATE
                ${PROJECT_NAME}::sql
                GTest::gtest_main)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    # Misuse that must not compile. EXCLUDE_FROM_ALL so the normal build
    # never touches it; ctest builds it and expects the failure.
    function(sql_add_compile_fail_test name)
        add_executable(${name} EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/${name}.cpp")
        target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        target_link_libraries(${name} PRIVATE ${PROJECT_NAME}::sql)
        add_test(NAME ${name}
                COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}"
                        --target ${name} --config $<CONFIG>)
        set_tests_properties(${name} PROPERTIES WILL_FAIL TRUE)
    endfunction()

    sql_add_test(test_error)
    sql_add_test(test_convert)
    sql_add_test(test_placeholders)
    sql_add_test(test_io)
endif ()
