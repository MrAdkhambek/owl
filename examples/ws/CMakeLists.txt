cmake_minimum_required(VERSION 4.3)
project(owl_ws_example CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# owl as a subproject, the way an application vendors it. No tests, no
# sql/redis: this example is only the WebSocket routes.
option(BUILD_TESTING "owl's own tests; off for a consumer" OFF)
set(OWL_ENABLE_SQLITE OFF CACHE BOOL "not used here")
set(OWL_ENABLE_POSTGRESQL OFF CACHE BOOL "not used here")
set(OWL_ENABLE_REDIS OFF CACHE BOOL "not used here")
add_subdirectory(../.. owl EXCLUDE_FROM_ALL)

add_executable(ws src/main.cpp)
target_link_libraries(ws PRIVATE owl::owl)
