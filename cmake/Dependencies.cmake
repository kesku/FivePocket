# SPDX-License-Identifier: GPL-3.0-or-later
include(FetchContent)
option(FIVEPOCKET_FETCH_DEPENDENCIES "Fetch pinned public SDK dependencies when paths are not supplied" ON)
set(FIVEPOCKET_SIMDE_ROOT "" CACHE PATH "SIMDe include root containing x86/ and simde-common.h")
set(FIVEPOCKET_MOLTENVK_ROOT "" CACHE PATH "Extracted official MoltenVK iOS SDK root")
set(FIVEPOCKET_FFMPEG_ROOT "" CACHE PATH "Source-built iOS FFmpeg prefix or build tree")

macro(fivepocket_dependencies)
    foreach(_resolved IN ITEMS _ffmpeg_headers _avcodec _avutil _moltenvk_headers _moltenvk)
        unset(${_resolved})
        unset(${_resolved} CACHE)
    endforeach()
    if(NOT FIVEPOCKET_FFMPEG_ROOT)
        message(FATAL_ERROR "Set FIVEPOCKET_FFMPEG_ROOT to the iOS output of tools/own_disc/bootstrap.py ffmpeg.")
    endif()
    find_path(_ffmpeg_headers libavcodec/avcodec.h PATHS
        "${FIVEPOCKET_FFMPEG_ROOT}" "${FIVEPOCKET_FFMPEG_ROOT}/include" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH NO_CACHE REQUIRED)
    find_library(_avcodec NAMES avcodec PATHS "${FIVEPOCKET_FFMPEG_ROOT}/libavcodec"
        "${FIVEPOCKET_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH NO_CACHE REQUIRED)
    find_library(_avutil NAMES avutil PATHS "${FIVEPOCKET_FFMPEG_ROOT}/libavutil"
        "${FIVEPOCKET_FFMPEG_ROOT}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH NO_CACHE REQUIRED)
    add_library(fivepocket_ffmpeg INTERFACE)
    target_include_directories(fivepocket_ffmpeg INTERFACE "${_ffmpeg_headers}")
    target_link_libraries(fivepocket_ffmpeg INTERFACE "${_avcodec}" "${_avutil}")

    if(NOT FIVEPOCKET_SIMDE_ROOT)
        if(NOT FIVEPOCKET_FETCH_DEPENDENCIES)
            message(FATAL_ERROR "Set FIVEPOCKET_SIMDE_ROOT or allow dependency fetching.")
        endif()
        FetchContent_Declare(fivepocket_simde_source
            GIT_REPOSITORY https://github.com/simd-everywhere/simde-no-tests.git
            GIT_TAG a532a12ca9bbdc5e6547eb602e6256b71a5377d4
            GIT_SUBMODULES "")
        FetchContent_MakeAvailable(fivepocket_simde_source)
        set(FIVEPOCKET_SIMDE_ROOT "${fivepocket_simde_source_SOURCE_DIR}")
    endif()
    if(NOT EXISTS "${FIVEPOCKET_SIMDE_ROOT}/simde-common.h")
        message(FATAL_ERROR "SIMDe root must contain simde-common.h and x86/ headers.")
    endif()
    add_library(fivepocket_simde INTERFACE)
    target_include_directories(fivepocket_simde INTERFACE "${FIVEPOCKET_SIMDE_ROOT}")

    if(NOT FIVEPOCKET_MOLTENVK_ROOT)
        if(NOT FIVEPOCKET_FETCH_DEPENDENCIES)
            message(FATAL_ERROR "Set FIVEPOCKET_MOLTENVK_ROOT or allow dependency fetching.")
        endif()
        FetchContent_Declare(fivepocket_moltenvk_sdk
            URL https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-ios.tar
            URL_HASH SHA256=b5d947b1660e6e9fed40b9cd2387e160aaab9e80b775c0cef7e14059405178c1
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(fivepocket_moltenvk_sdk)
        set(FIVEPOCKET_MOLTENVK_ROOT "${fivepocket_moltenvk_sdk_SOURCE_DIR}")
    endif()
    find_path(_moltenvk_headers vulkan/vulkan.h PATHS "${FIVEPOCKET_MOLTENVK_ROOT}"
        PATH_SUFFIXES include MoltenVK/include NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH NO_CACHE REQUIRED)
    find_library(_moltenvk NAMES MoltenVK PATHS "${FIVEPOCKET_MOLTENVK_ROOT}"
        PATH_SUFFIXES MoltenVK/static/MoltenVK.xcframework/ios-arm64
        static/MoltenVK.xcframework/ios-arm64 MoltenVK.xcframework/ios-arm64
        MoltenVK/MoltenVK.xcframework/ios-arm64 NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH NO_CACHE REQUIRED)
    add_library(fivepocket_moltenvk INTERFACE)
    target_include_directories(fivepocket_moltenvk INTERFACE "${_moltenvk_headers}")
    target_link_libraries(fivepocket_moltenvk INTERFACE "${_moltenvk}")
endmacro()
