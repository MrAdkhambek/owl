add_library(fstr INTERFACE)
add_library(${PROJECT_NAME}::fstr ALIAS fstr)

target_compile_features(fstr INTERFACE cxx_std_23)

target_include_directories(fstr INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_sources(fstr INTERFACE
        FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/include/fstr/fstr.h"
)

if (BUILD_TESTING)
    function(fstr_add_test name)
        add_executable(${name} "${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}.cpp")
        target_link_libraries(${name}
                PRIVATE
                ${PROJECT_NAME}::fstr
                GTest::gtest_main)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    fstr_add_test(test_fstr)

    function(fstr_add_compile_fail_test name)
        add_executable(${name} EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/tests/compile_fail/${name}.cpp")
        target_link_libraries(${name} PRIVATE ${PROJECT_NAME}::fstr)
        add_test(NAME ${name}
                COMMAND ${CMAKE_COMMAND} --build "${CMAKE_BINARY_DIR}"
                        --target ${name} --config $<CONFIG>)
        set_tests_properties(${name} PROPERTIES WILL_FAIL TRUE)
    endfunction()

    fstr_add_compile_fail_test(rejects_runtime_buffer)
    fstr_add_compile_fail_test(rejects_concatenation)
    fstr_add_compile_fail_test(rejects_embedded_nul)
endif ()
