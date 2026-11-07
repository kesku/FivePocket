# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(FivePocketRuntimeTests LANGUAGES CXX)
enable_testing()
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

function(add_runtime_test name)
    add_executable(${name} ${ARGN})
    target_compile_features(${name} PRIVATE cxx_std_20)
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_link_libraries(${name} PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /WX /UNDEBUG)
    else()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_runtime_test(diagnostic_logging diagnostic_logging_test.cpp)
set_tests_properties(diagnostic_logging PROPERTIES ENVIRONMENT "RECOMP_VERBOSE_DIAGNOSTICS=0")
add_test(NAME diagnostic_logging_verbose COMMAND diagnostic_logging)
set_tests_properties(diagnostic_logging_verbose PROPERTIES ENVIRONMENT "RECOMP_VERBOSE_DIAGNOSTICS=1")
add_test(NAME diagnostic_logging_invalid_option COMMAND diagnostic_logging)
set_tests_properties(diagnostic_logging_invalid_option PROPERTIES ENVIRONMENT "RECOMP_VERBOSE_DIAGNOSTICS=true")
add_runtime_test(diagnostic_logging_play diagnostic_logging_test.cpp)
target_compile_definitions(diagnostic_logging_play PRIVATE RECOMP_PLAY_BUILD=1)
set_tests_properties(diagnostic_logging_play PROPERTIES ENVIRONMENT "RECOMP_VERBOSE_DIAGNOSTICS=0")
add_test(NAME diagnostic_logging_play_verbose COMMAND diagnostic_logging_play)
set_tests_properties(diagnostic_logging_play_verbose PROPERTIES ENVIRONMENT "RECOMP_VERBOSE_DIAGNOSTICS=1")
add_runtime_test(apple_stereo_ring apple_stereo_ring_test.cpp)
add_runtime_test(touch_diagnostics touch_diagnostics_test.cpp)
target_include_directories(touch_diagnostics PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../app/duo")
set_tests_properties(touch_diagnostics PROPERTIES ENVIRONMENT "RECOMP_VERBOSE_DIAGNOSTICS=0")
add_test(NAME touch_diagnostics_default COMMAND "${CMAKE_COMMAND}" -E env
    --unset=RECOMP_VERBOSE_DIAGNOSTICS $<TARGET_FILE:touch_diagnostics>)
add_test(NAME touch_diagnostics_verbose COMMAND touch_diagnostics)
set_tests_properties(touch_diagnostics_verbose PROPERTIES ENVIRONMENT "RECOMP_VERBOSE_DIAGNOSTICS=1")
add_test(NAME touch_diagnostics_invalid_option COMMAND touch_diagnostics)
set_tests_properties(touch_diagnostics_invalid_option PROPERTIES ENVIRONMENT "RECOMP_VERBOSE_DIAGNOSTICS=true")

add_runtime_test(executor_timing executor_timing_test.cpp
    ../guest_cooperative_executor.cpp ../guest_execution_guard.cpp)
target_compile_definitions(executor_timing PRIVATE RECOMP_GUEST_THREADS=2)
target_include_directories(executor_timing PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../host"
    "${CMAKE_CURRENT_SOURCE_DIR}/../bridge"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../platform/apple/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../app/duo")
set_tests_properties(executor_timing PROPERTIES TIMEOUT 20)

add_test(NAME pipeline_cache_stencil COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/pipeline_cache_regression.py" --compiler "${CMAKE_CXX_COMPILER}"
    --out "${CMAKE_CURRENT_BINARY_DIR}/pipeline-cache-regression")
