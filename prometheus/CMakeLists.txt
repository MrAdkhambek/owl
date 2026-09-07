add_library(prometheus INTERFACE)
add_library(${PROJECT_NAME}::prometheus ALIAS prometheus)

target_compile_features(prometheus INTERFACE cxx_std_23)

target_include_directories(prometheus INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_sources(prometheus INTERFACE
        FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/include/prometheus/prometheus.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/prometheus/http.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/prometheus/registry.h"
)

if (BUILD_TESTING)
    add_executable(test_metrics "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_metrics.cpp")
    target_link_libraries(test_metrics
            PRIVATE
            ${PROJECT_NAME}::prometheus
            GTest::gtest_main)
    add_test(NAME test_metrics COMMAND test_metrics)
endif ()
