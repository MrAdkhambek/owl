cmake_minimum_required(VERSION 4.3)
project(owl)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CTest)
include(cmake/owl_header_guard.cmake)

if (PROJECT_IS_TOP_LEVEL)
    # A coroutine that falls off its end without co_return finishes with no
    # result, and clang only warns; here that is an error, for this
    # project's own tests and benches, never for a consumer.
    add_compile_options($<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>:-Werror=return-type>)
endif ()

option(OWL_ENABLE_PROMETHEUS "Build owl::prometheus (HTTP RED + scrape handler)" OFF)
option(OWL_ENABLE_SQLITE "Build owl::sql sqlite driver (needs sqlite3)" ON)
option(OWL_ENABLE_POSTGRESQL "Build owl::sql postgres driver" OFF)

if (BUILD_TESTING)
    include(FetchContent)
    FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.17.0
            GIT_SHALLOW TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif ()

if (OWL_ENABLE_PROMETHEUS)
    # Added before owl so the owl target can link it.
    add_subdirectory(prometheus)
endif ()

if (OWL_ENABLE_SQLITE OR OWL_ENABLE_POSTGRESQL)
    # Added before owl so the owl target can link it.
    add_subdirectory(sql)
endif ()

add_subdirectory(coro)
add_subdirectory(fstr)
add_subdirectory(owl)


set(_owl_install_targets coro fstr owl)
if (OWL_ENABLE_PROMETHEUS)
    list(APPEND _owl_install_targets prometheus)
endif ()
if (OWL_ENABLE_SQLITE OR OWL_ENABLE_POSTGRESQL)
    list(APPEND _owl_install_targets sql)
endif ()
install(TARGETS ${_owl_install_targets} FILE_SET HEADERS)

include(CMakePackageConfigHelpers)
configure_package_config_file(
        cmake/owlConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/owlConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/owl"
        PATH_VARS CMAKE_INSTALL_INCLUDEDIR
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/owlConfig.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/owl")
