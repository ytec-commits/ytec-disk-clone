cmake_minimum_required(VERSION 3.25)

project(
  YtecDiskClone
  VERSION 0.2.0
  DESCRIPTION "Y-TEC Windows disk clone and image backup tool"
  LANGUAGES C CXX
)

if(NOT WIN32)
  message(FATAL_ERROR "Y-TEC Tsumugi Drive supports Windows only.")
endif()

if(NOT MSVC)
  message(FATAL_ERROR "Y-TEC Tsumugi Drive requires the MSVC toolchain.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Y-TEC Tsumugi Drive must be configured for x64.")
endif()

# MSVC 19.50 can emit /showIncludes as UTF-8 even when CMake 3.29 decodes the
# Japanese prefix as CP932.  Ninja then records zero header dependencies and
# can link ABI-incompatible stale objects after a public header change.
# Correct only the known mojibake value; English and other valid prefixes are
# left untouched.
if(
  CMAKE_GENERATOR MATCHES "Ninja"
  AND CMAKE_CL_SHOWINCLUDES_PREFIX MATCHES "^繝"
)
  set(CMAKE_CL_SHOWINCLUDES_PREFIX "メモ: インクルード ファイル: ")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(YTEC_ENABLE_ASAN "Enable MSVC AddressSanitizer instrumentation" OFF)
option(YTEC_ENABLE_MSVC_ANALYZE "Enable MSVC native code analysis" OFF)
option(
  YTEC_BUILD_DESTRUCTIVE_VM_TESTS
  "Build VM-only destructive disk integration harnesses"
  OFF
)

include(CTest)
include(cmake/YtecWarnings.cmake)

set(ZSTD_BUILD_STATIC ON CACHE BOOL "Build bundled static Zstandard" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "Do not build a Zstandard DLL" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "Do not build the Zstandard CLI" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "Use project-owned Zstandard tests" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "Do not build Zstandard contrib" FORCE)
set(ZSTD_BUILD_DICTBUILDER OFF CACHE BOOL "No dictionaries in product image formats" FORCE)
set(ZSTD_BUILD_DEPRECATED OFF CACHE BOOL "No deprecated Zstandard API" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "Reject legacy Zstandard frames" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "Use bounded single-thread chunks" FORCE)
add_subdirectory(third_party/zstd/build/cmake EXCLUDE_FROM_ALL)
add_library(ytec::zstd ALIAS libzstd_static)
target_compile_options(libzstd_static PRIVATE /utf-8)
if(YTEC_ENABLE_ASAN)
  target_compile_options(libzstd_static PRIVATE /fsanitize=address)
endif()

add_subdirectory(third_party/argon2 EXCLUDE_FROM_ALL)

add_subdirectory(src)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
