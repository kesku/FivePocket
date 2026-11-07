# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.24)
project(FivePocketHostTools LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(BUILD_SHARED_LIBS OFF)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)

foreach(required IN ITEMS XENON_SOURCE XENOS_SOURCE_ROOT DXC_SOURCE DXC_LIBRARY HOST_OUTPUT)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} must be supplied by bootstrap.py")
  endif()
endforeach()

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_subdirectory("${XENON_SOURCE}" xenon)
set_target_properties(XenonRecomp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${HOST_OUTPUT}")
add_executable(xenon-fragment fragment.cpp "${XENON_SOURCE}/XenonRecomp/recompiler.cpp"
  "${XENON_SOURCE}/XenonRecomp/recompiler_config.cpp")
target_include_directories(xenon-fragment PRIVATE "${XENON_SOURCE}/XenonRecomp")
target_precompile_headers(xenon-fragment PRIVATE "${XENON_SOURCE}/XenonRecomp/pch.h")
target_link_libraries(xenon-fragment PRIVATE LibXenonAnalyse XenonUtils fmt::fmt tomlplusplus::tomlplusplus xxHash::xxhash)
set_target_properties(xenon-fragment PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${HOST_OUTPUT}")
add_executable(xex_image "${REPO}/tools/xex_image/xex_image.cpp")
target_link_libraries(xex_image PRIVATE XenonUtils)
set_target_properties(xex_image PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${HOST_OUTPUT}")
add_executable(xmem-lzx "${REPO}/tools/xmem_lzx/main.cpp" "${REPO}/tools/xmem_lzx/xmem_lzx.cpp"
  "${XENON_SOURCE}/thirdparty/libmspack/libmspack/mspack/lzxd.c")
target_include_directories(xmem-lzx PRIVATE "${XENON_SOURCE}/thirdparty/libmspack/libmspack/mspack")
set_target_properties(xmem-lzx PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${HOST_OUTPUT}")

add_library(Microsoft::DirectXShaderCompiler SHARED IMPORTED GLOBAL)
set_target_properties(Microsoft::DirectXShaderCompiler PROPERTIES IMPORTED_LOCATION "${DXC_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${DXC_SOURCE}/include;${DXC_SOURCE}/include/dxc")
set(BASE "${XENOS_SOURCE_ROOT}/v41")
add_subdirectory("${BASE}/thirdparty/zstd/build/cmake" zstd)
set(SMOLV "${BASE}/thirdparty/smol-v/source")
foreach(mode IN ITEMS v41 main gradient reglod coissue coissue-reglod)
  set(source "${XENOS_SOURCE_ROOT}/${mode}/XenosRecomp")
  add_executable(xenos_${mode} "${source}/main.cpp" "${source}/dxc_compiler.cpp"
    "${source}/shader_recompiler.cpp" "${source}/shader_validator.cpp" "${SMOLV}/smolv.cpp")
  target_include_directories(xenos_${mode} PRIVATE "${source}" "${SMOLV}")
  target_precompile_headers(xenos_${mode} PRIVATE "${source}/pch.h")
  target_link_libraries(xenos_${mode} PRIVATE Microsoft::DirectXShaderCompiler xxHash::xxhash libzstd_static fmt::fmt)
  target_compile_options(xenos_${mode} PRIVATE -Wno-switch -Wno-unused-variable -Wno-null-arithmetic -fms-extensions)
  set_target_properties(xenos_${mode} PROPERTIES OUTPUT_NAME XenosRecomp
    RUNTIME_OUTPUT_DIRECTORY "${HOST_OUTPUT}/xenos/${mode}")
  add_custom_command(TARGET xenos_${mode} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${source}/shader_common.h" "${HOST_OUTPUT}/xenos/${mode}/shader_common.h")
endforeach()
add_executable(XenosShaderPreflight "${BASE}/XenosRecomp/shader_validator.cpp"
  "${BASE}/XenosRecomp/shader_validator_batch.cpp" "${BASE}/XenosRecomp/shader_validator_cli.cpp")
target_include_directories(XenosShaderPreflight PRIVATE "${BASE}/XenosRecomp")
set_target_properties(XenosShaderPreflight PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${HOST_OUTPUT}/xenos/main")
