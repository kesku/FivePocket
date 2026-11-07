# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(FivePocketDuo VERSION 1.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OBJCXX_EXTENSIONS OFF)

include(CTest)
set(_duo_default OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(_duo_default ON)
endif()
option(FIVEPOCKET_BUILD_DUO "Build the iPhone Duo application" ${_duo_default})

if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(runtime/tests)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME generated_source_contract COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/test_generated_inputs.py")
endif()
if(NOT FIVEPOCKET_BUILD_DUO)
    message(STATUS "Host checks only. Use cmake/iOS.cmake as the toolchain to build the Duo app.")
    return()
endif()
if(NOT APPLE OR NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "The Duo target requires the iOS SDK. Configure with -DCMAKE_TOOLCHAIN_FILE=cmake/iOS.cmake")
endif()
enable_language(OBJCXX)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(FIVEPOCKET_GAME_DIR "" CACHE PATH "Private source-generation output containing build-inputs.cmake")
set(FIVEPOCKET_BUNDLE_IDENTIFIER "com.fivepocket.duo" CACHE STRING "Bundle identifier used to sign and install this app")
set(FIVEPOCKET_DEVELOPMENT_TEAM "" CACHE STRING "Optional Apple development team; empty builds an unsigned app")
set(FIVEPOCKET_DEVELOPER_DIR "$ENV{DEVELOPER_DIR}" CACHE PATH "Xcode developer directory used for asset compilation")
include(cmake/GeneratedInputs.cmake)
fivepocket_load_generated_inputs()
include(cmake/Dependencies.cmake)
fivepocket_dependencies()
include(cmake/DuoSources.cmake)

set(_labels "${CMAKE_CURRENT_BINARY_DIR}/generated-ui")
find_program(FIVEPOCKET_XCRUN xcrun REQUIRED)
add_custom_command(
    OUTPUT "${_labels}/loading_label.h" "${_labels}/map_tap_label.h"
    COMMAND "${FIVEPOCKET_XCRUN}" --sdk macosx swift
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/own_disc/generate_ui_labels.swift"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/duo/NunitoSans.ttf" "${_labels}"
    DEPENDS tools/own_disc/generate_ui_labels.swift resources/duo/NunitoSans.ttf
    VERBATIM)
add_custom_target(fivepocket_ui_labels DEPENDS "${_labels}/loading_label.h" "${_labels}/map_tap_label.h")

add_library(fivepocket_options INTERFACE)
target_compile_features(fivepocket_options INTERFACE cxx_std_20)
target_compile_options(fivepocket_options INTERFACE
    "$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-UNDEBUG;-fno-strict-aliasing;-fwrapv;-ffp-model=strict>"
    "$<$<NOT:$<CONFIG:Debug>>:-O2>"
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc;-fblocks>")
target_include_directories(fivepocket_options INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/app/duo"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/apple"
    "${CMAKE_CURRENT_SOURCE_DIR}/platform/apple/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/host"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/bridge"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime/gpu"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr1"
    "${_labels}"
    ${FIVEPOCKET_GENERATED_INCLUDE_DIRS})
target_link_libraries(fivepocket_options INTERFACE fivepocket_simde fivepocket_moltenvk)

add_library(fivepocket_runtime OBJECT ${FIVEPOCKET_RUNTIME_SOURCES})
target_link_libraries(fivepocket_runtime PRIVATE fivepocket_options fivepocket_ffmpeg)
add_dependencies(fivepocket_runtime fivepocket_ui_labels)
target_compile_definitions(fivepocket_runtime PRIVATE
    RECOMP_PRIVATE_TRACE_LIBRARY=1 SIMDE_ENABLE_NATIVE_ALIASES host_guest_trace_EXPORTS _FORTIFY_SOURCE=2)
target_compile_options(fivepocket_runtime PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/runtime/bridge/trace_prelude.h")
fivepocket_runtime_definitions()

add_library(fivepocket_game OBJECT ${FIVEPOCKET_GENERATED_SOURCES})
target_link_libraries(fivepocket_game PRIVATE fivepocket_options)
target_compile_definitions(fivepocket_game PRIVATE
    RECOMP_PRIVATE_TRACE_LIBRARY=1 SIMDE_ENABLE_NATIVE_ALIASES host_guest_trace_EXPORTS _FORTIFY_SOURCE=2)
target_compile_options(fivepocket_game PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/runtime/bridge/trace_prelude.h")
foreach(source IN LISTS FIVEPOCKET_GENERATED_SOURCES)
    get_filename_component(name "${source}" NAME)
    if(name MATCHES "^ppc_recomp[.].*[.]cpp$")
        set_property(SOURCE "${source}" APPEND PROPERTY INCLUDE_DIRECTORIES
            "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ppc")
        set_property(SOURCE "${source}" APPEND PROPERTY COMPILE_DEFINITIONS
            RECOMP_GUEST_MEMORY_ATOMIC=1 RECOMP_GUEST_SPECIAL_CONST_WINDOWS=1
            RECOMP_GUEST_SPECIAL_CONST_WINDOWS_TREE=1 RECOMP_GUEST_LEAN_RAW=1)
    endif()
endforeach()

add_executable(FivePocketDuo MACOSX_BUNDLE ${FIVEPOCKET_APPLE_SOURCES})
target_link_libraries(FivePocketDuo PRIVATE fivepocket_options fivepocket_ffmpeg fivepocket_runtime fivepocket_game)
add_dependencies(FivePocketDuo fivepocket_ui_labels)
target_compile_options(FivePocketDuo PRIVATE -fblocks)
fivepocket_apple_definitions()
foreach(framework IN ITEMS Metal Foundation QuartzCore IOSurface CoreGraphics AudioToolbox
        CoreAudio UIKit AVFoundation AVFAudio GameController CoreText UniformTypeIdentifiers)
    find_library(_framework_${framework} "${framework}" REQUIRED)
    target_link_libraries(FivePocketDuo PRIVATE "${_framework_${framework}}")
endforeach()
# Keep the optional profile-dump hooks available without enabling instrumentation.
set(_profile_query "${CMAKE_CXX_COMPILER}" -target "arm64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}"
    -print-file-name=libclang_rt.profile_ios.a)
if(FIVEPOCKET_DEVELOPER_DIR)
    list(PREPEND _profile_query "${CMAKE_COMMAND}" -E env "DEVELOPER_DIR=${FIVEPOCKET_DEVELOPER_DIR}")
endif()
execute_process(COMMAND ${_profile_query} RESULT_VARIABLE _profile_result
    OUTPUT_VARIABLE _profile_runtime OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_VARIABLE _profile_error)
if(NOT _profile_result EQUAL 0 OR NOT EXISTS "${_profile_runtime}")
    message(FATAL_ERROR "Selected Xcode has no iOS profile runtime: ${_profile_error}")
endif()
target_link_libraries(FivePocketDuo PRIVATE z "${_profile_runtime}")
target_link_options(FivePocketDuo PRIVATE -fobjc-arc -fobjc-link-runtime -Wl,-export_dynamic)
if(NOT FIVEPOCKET_DEVELOPMENT_TEAM)
    target_link_options(FivePocketDuo PRIVATE -Wl,-no_adhoc_codesign)
endif()

set(_resources resources/duo/NunitoSans.ttf resources/duo/NunitoSans-OFL.txt)
target_sources(FivePocketDuo PRIVATE ${_resources} ${FIVEPOCKET_GENERATED_RESOURCES})
set_source_files_properties(${_resources} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
foreach(resource IN LISTS FIVEPOCKET_GENERATED_RESOURCES)
    file(RELATIVE_PATH relative "${FIVEPOCKET_GENERATED_RESOURCE_ROOT}" "${resource}")
    if(relative MATCHES "^[.][.]/")
        message(FATAL_ERROR "Generated app resource lies outside its resource root: ${resource}")
    endif()
    get_filename_component(folder "${relative}" DIRECTORY)
    set(location Resources)
    cmake_path(APPEND location "${FIVEPOCKET_GENERATED_RESOURCE_PREFIX}" "${folder}")
    set_source_files_properties("${resource}" PROPERTIES MACOSX_PACKAGE_LOCATION "${location}")
endforeach()
set_target_properties(FivePocketDuo PROPERTIES
    OUTPUT_NAME FivePocketDuo
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DuoInfo.plist.in"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${FIVEPOCKET_BUNDLE_IDENTIFIER}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    XCODE_GENERATE_SCHEME YES
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${FIVEPOCKET_BUNDLE_IDENTIFIER}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY 1
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)
if(FIVEPOCKET_DEVELOPMENT_TEAM)
    set_target_properties(FivePocketDuo PROPERTIES
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED YES
        XCODE_ATTRIBUTE_CODE_SIGN_STYLE Automatic
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${FIVEPOCKET_DEVELOPMENT_TEAM}")
endif()
add_custom_command(TARGET FivePocketDuo POST_BUILD
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_icon.py"
        --app "$<TARGET_BUNDLE_DIR:FivePocketDuo>"
        --icon "${CMAKE_CURRENT_SOURCE_DIR}/resources/duo/AppIcon.png"
        --minimum "${CMAKE_OSX_DEPLOYMENT_TARGET}"
        --developer "${FIVEPOCKET_DEVELOPER_DIR}"
    VERBATIM)
set_property(TARGET FivePocketDuo APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/duo/AppIcon.png"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_icon.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/own_disc/ios_app.py")

add_custom_target(ipa
    COMMAND "${CMAKE_COMMAND}" -E rm -f "${CMAKE_CURRENT_BINARY_DIR}/FivePocketDuo.ipa"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/own_disc/ipa_writer.py"
        --app "$<TARGET_BUNDLE_DIR:FivePocketDuo>" --output "${CMAKE_CURRENT_BINARY_DIR}/FivePocketDuo.ipa"
    DEPENDS FivePocketDuo
    VERBATIM)
