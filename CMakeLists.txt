cmake_minimum_required(VERSION 3.20)

project(overlaybd-elio
    VERSION 0.1.0
    DESCRIPTION "OverlayBD-compatible lazy-loading block device on Elio, with ublk backend, DART P2P source and per-device process isolation"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(OBD_BUILD_TESTS "Build unit and integration tests" ON)
option(OBD_ENABLE_UBLK "Build the ublk block-device backend (requires linux/ublk_cmd.h and liburing)" ON)
option(OBD_ENABLE_ZSTD "Enable Zstd compression support in ZFile (OverlayBD algo 2)" ON)
option(OBD_ENABLE_LIBE2FS_BACKEND "Build obd-convert with the pinned libe2fs converter backend" ON)
option(OBD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Third-party: fetched dependencies
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# Elio coroutine runtime, pinned by commit (network fetch; configure behind a
# proxy with https_proxy if needed). Bump in a dedicated commit with a reason.
set(ELIO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ELIO_BUILD_SANITIZER_TESTS OFF CACHE BOOL "" FORCE)
set(ELIO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ELIO_BUILD_TCP_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(ELIO_ENABLE_TLS ON CACHE BOOL "" FORCE)
set(ELIO_ENABLE_HTTP ON CACHE BOOL "" FORCE)
set(ELIO_ENABLE_HTTP2 OFF CACHE BOOL "" FORCE)
FetchContent_Declare(elio
    GIT_REPOSITORY https://github.com/Coldwings/Elio.git
    GIT_TAG 7b65e0b4ed0234ba699a05f7478d7b0f2f0b38ea)
FetchContent_MakeAvailable(elio)

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(lz4
    URL https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.tar.gz
    SOURCE_SUBDIR build/cmake)
set(LZ4_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LZ4_BUILD_LEGACY_LZ4C OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
set(LZ4_POSITION_INDEPENDENT_LIB ON)
FetchContent_MakeAvailable(lz4)
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})

find_package(ZLIB REQUIRED)

if(OBD_ENABLE_LIBE2FS_BACKEND)
    enable_language(C)
    include(ObdLibE2fs)
endif()

if(OBD_ENABLE_ZSTD)
    FetchContent_Declare(zstd
        URL https://github.com/facebook/zstd/releases/download/v1.5.6/zstd-1.5.6.tar.gz
        SOURCE_SUBDIR build/cmake)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zstd)
endif()

# liburing: reuse Elio's imported target when present, else import ourselves.
if(NOT TARGET liburing::liburing)
    find_library(URING_LIBRARY NAMES uring REQUIRED)
    find_path(URING_INCLUDE_DIR NAMES liburing.h REQUIRED)
    add_library(liburing::liburing UNKNOWN IMPORTED)
    set_target_properties(liburing::liburing PROPERTIES
        IMPORTED_LOCATION "${URING_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${URING_INCLUDE_DIR}")
endif()

if(OBD_ENABLE_UBLK)
    include(CheckIncludeFileCXX)
    check_include_file_cxx("linux/ublk_cmd.h" HAVE_LINUX_UBLK_CMD_H)
    if(NOT HAVE_LINUX_UBLK_CMD_H)
        message(FATAL_ERROR
            "OBD_ENABLE_UBLK=ON but <linux/ublk_cmd.h> is missing; "
            "install kernel headers >= 6.0 or configure with -DOBD_ENABLE_UBLK=OFF")
    endif()
endif()

# ---------------------------------------------------------------------------
# Project targets
# ---------------------------------------------------------------------------
include(ProjectWarnings)

add_subdirectory(src)
add_subdirectory(tools)

if(OBD_BUILD_TESTS)
    enable_testing()
    FetchContent_Declare(Catch2
        URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.5.4.tar.gz)
    FetchContent_MakeAvailable(Catch2)
    add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "overlaybd-elio ${PROJECT_VERSION} configuration:")
message(STATUS "  build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  tests           : ${OBD_BUILD_TESTS}")
message(STATUS "  ublk backend    : ${OBD_ENABLE_UBLK}")
message(STATUS "  libe2fs backend : ${OBD_ENABLE_LIBE2FS_BACKEND}")
message(STATUS "")
