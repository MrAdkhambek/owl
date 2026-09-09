cmake_minimum_required(VERSION 4.3)
project(owl_rest_example CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# owl as a subproject, the way an application vendors it: the repository
# two levels up, built here as a consumer -- no tests, sqlite on, the
# other drivers off. After `cmake --install` of owl, the same two targets
# come from `find_package(owl REQUIRED)` instead.
option(BUILD_TESTING "owl's own tests; off for a consumer" OFF)
set(OWL_ENABLE_SQLITE ON CACHE BOOL "the example stores its data in sqlite")
add_subdirectory(../.. owl EXCLUDE_FROM_ALL)

# One translation unit per module, axum style; headers include each other
# as "rest/...", so a routes/auth.h never shadows the top-level auth.h.
add_executable(rest
        src/rest/main.cpp
        src/rest/auth.cpp
        src/rest/db.cpp
        src/rest/routes/auth.cpp
        src/rest/routes/posts.cpp)
target_include_directories(rest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(rest PRIVATE owl::owl owl::sql)
