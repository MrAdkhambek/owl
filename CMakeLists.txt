cmake_minimum_required(VERSION 4.3)
project(owl)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CTest)

option(OWL_ENABLE_PROMETHEUS "Build owl::prometheus (HTTP RED + scrape handler)" OFF)

add_executable(${PROJECT_NAME}_demo main.cpp)

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

add_subdirectory(coro)
add_subdirectory(fstr)
add_subdirectory(owl)

target_link_libraries(${PROJECT_NAME}_demo PRIVATE
        ${PROJECT_NAME}::coro
        ${PROJECT_NAME}::fstr
        ${PROJECT_NAME}::owl)

if (OWL_ENABLE_PROMETHEUS)
    install(TARGETS coro fstr owl prometheus FILE_SET HEADERS)
else ()
    install(TARGETS coro fstr owl FILE_SET HEADERS)
endif ()

include(CMakePackageConfigHelpers)
configure_package_config_file(
        cmake/owlConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/owlConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/owl"
        PATH_VARS CMAKE_INSTALL_INCLUDEDIR
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/owlConfig.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/owl")
