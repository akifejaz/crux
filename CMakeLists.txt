cmake_minimum_required(VERSION 3.25)

# vcpkg toolchain support (optional). If VCPKG_ROOT is set and the user did
# not pass CMAKE_TOOLCHAIN_FILE, we hook it in automatically. If vcpkg is not
# present, we fall back to CMake FetchContent for all deps.
if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE STRING "Vcpkg toolchain file")
endif()

project(crux LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# Prefer vcpkg-supplied packages; otherwise pull via FetchContent so the tree
# builds standalone.
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

find_package(CLI11 CONFIG QUIET)
if(NOT CLI11_FOUND)
    FetchContent_Declare(CLI11
        GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
        GIT_TAG v2.4.2
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(CLI11)
endif()

find_package(nlohmann_json CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(spdlog CONFIG QUIET)
if(NOT spdlog_FOUND)
    set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.14.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(spdlog)
endif()

find_package(doctest CONFIG QUIET)
if(NOT doctest_FOUND)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(doctest)
endif()

# cpp-httplib for the local dashboard server (single header).
find_package(httplib CONFIG QUIET)
if(NOT httplib_FOUND)
    set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_REQUIRE_ZLIB    OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_COMPILE         OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(httplib
        GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
        GIT_TAG v0.15.3
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(httplib)
endif()

# ---------------------------------------------------------------------------
# Core library (pure functions, no I/O — unit-testable)
# ---------------------------------------------------------------------------
add_library(crux_core STATIC
    src/core/detector.cpp
    src/core/planner.cpp
    src/core/caption_parse.cpp
    src/core/caption_lexicon.cpp
    src/core/caption_scorer.cpp
    src/fetch/ytdlp_source.cpp
    src/fetch/subs.cpp
    src/fetch/parse.cpp
    src/media/proc.cpp
    src/media/downloader.cpp
    src/media/cutter.cpp
    src/media/intro_assets.cpp
    src/media/outro_card.cpp
    src/media/silence.cpp
    src/media/subtitle_render.cpp
    src/media/transliterate.cpp
    src/out/manifest.cpp
    src/pipeline.cpp
    src/cli.cpp
    src/binres.cpp
)
target_include_directories(crux_core PUBLIC src)
target_link_libraries(crux_core PUBLIC
    CLI11::CLI11
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

# ---------------------------------------------------------------------------
# Executables
# ---------------------------------------------------------------------------
add_executable(crux src/main.cpp)
target_link_libraries(crux PRIVATE crux_core)

# Local dashboard server. Small and standalone — links against the media/proc
# subprocess helper so it can spawn crux.exe with output redirected to a
# file that the HTTP endpoints tail.
add_executable(crux_server src/server/server.cpp)
target_link_libraries(crux_server PRIVATE
    crux_core
    httplib::httplib
    nlohmann_json::nlohmann_json
)
if(WIN32)
    target_link_libraries(crux_server PRIVATE ws2_32)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_executable(crux_tests
    tests/main_test.cpp
    tests/parse_test.cpp
    tests/parse_edge_test.cpp
    tests/detector_test.cpp
    tests/detector_extra_test.cpp
    tests/planner_test.cpp
    tests/cli_test.cpp
    tests/manifest_test.cpp
    tests/proc_test.cpp
    tests/downloader_strategy_test.cpp
    tests/captions_test.cpp
    tests/transliterate_test.cpp
)
target_link_libraries(crux_tests PRIVATE crux_core doctest::doctest)
target_compile_definitions(crux_tests PRIVATE
    CRUX_FIXTURES_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures"
    CRUX_CMAKE_COMMAND="${CMAKE_COMMAND}"
)
add_test(NAME crux_tests COMMAND crux_tests)

# Standalone verifier — no external deps, buildable with plain g++.
# Useful in sandboxes and as a quick CI smoke.
add_executable(crux_standalone_verify
    tests/standalone_verify.cpp
    src/core/detector.cpp
    src/core/planner.cpp)
target_include_directories(crux_standalone_verify PRIVATE src)
add_test(NAME crux_standalone_verify COMMAND crux_standalone_verify)
