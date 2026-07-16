// Subprocess wrapper — regression tests for src/media/proc.cpp.
//
// Uses `cmake -E` for portability: CMake is guaranteed to be on PATH in the
// build environment, its -E subcommands behave identically on Windows / POSIX,
// and there's no dependency on the platform shell (which differs on quoting).
//
// Covers: capture stdout, capture stderr, non-zero exit, redirect_output_to
// (both stdout+stderr → file), and timeout supervision.

#include <doctest/doctest.h>

#include "media/proc.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;

namespace {

// Prefer the cmake that built this binary (baked in as a compile def).
// Fall back to env / PATH if the define isn't set (e.g. hand-built).
std::string cmake_bin() {
#ifdef CRUX_CMAKE_COMMAND
    std::string baked{CRUX_CMAKE_COMMAND};
    if (!baked.empty()) return baked;
#endif
    if (const char* c = std::getenv("CMAKE_COMMAND")) return c;
#ifdef _WIN32
    return "cmake.exe";
#else
    return "cmake";
#endif
}

fs::path tempdir_for(const std::string& name) {
    auto p = fs::temp_directory_path() / ("crux_proctest_" + name);
    std::error_code ec;
    fs::remove_all(p, ec);   // ignore failures
    fs::create_directories(p, ec);
    return p;
}

// Windows keeps file HANDLEs alive briefly after CloseHandle returns, so a
// fresh remove_all can fail with sharing violations. Retry with backoff.
void best_effort_remove_all(const fs::path& p) {
    for (int i = 0; i < 20; ++i) {
        std::error_code ec;
        fs::remove_all(p, ec);
        if (!ec) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

} // namespace

TEST_CASE("proc::run captures stdout of a simple command") {
    crux::proc::RunResult r = crux::proc::run(
        cmake_bin(), {"-E", "echo", "hello", "crux"});
    CHECK(r.exit_code == 0);
    CHECK(r.stdout_text.find("hello crux") != std::string::npos);
}

TEST_CASE("proc::run reports non-zero exit codes") {
    // `cmake -E false` (like /bin/false) exits with 1.
    crux::proc::RunResult r = crux::proc::run(cmake_bin(), {"-E", "false"});
    CHECK(r.exit_code != 0);
}

TEST_CASE("proc::check_run throws on non-zero exit") {
    CHECK_THROWS_AS(
        crux::proc::check_run(cmake_bin(), {"-E", "false"}),
        std::exception);
}

TEST_CASE("proc::run — redirect_output_to writes both streams to file") {
    auto dir = tempdir_for("redirect");
    auto log = dir / "run.log";

    crux::proc::RunOptions opts;
    opts.redirect_output_to = log.string();
    // `cmake -E echo` prints to stdout — redirect should capture it.
    crux::proc::RunResult r = crux::proc::run(
        cmake_bin(), {"-E", "echo", "redirect-test-payload"}, opts);
    CHECK(r.exit_code == 0);

    REQUIRE(fs::exists(log));
    std::ifstream f(log);
    std::ostringstream oss; oss << f.rdbuf();
    CHECK(oss.str().find("redirect-test-payload") != std::string::npos);

    // Under redirect mode the RunResult captures nothing itself.
    CHECK(r.stdout_text.empty());
    CHECK(r.stderr_text.empty());
    best_effort_remove_all(dir);
}

TEST_CASE("proc::run — nonexistent binary throws") {
    CHECK_THROWS_AS(
        crux::proc::run("crux_definitely_not_a_real_binary_xyz", {"--help"}),
        std::exception);
}

TEST_CASE("proc::run — timeout kills long-running child (redirect mode)") {
    // Timeout is only reliable in redirect mode: the pipe-drain path blocks
    // on ReadFile until the child exits, so a silent child (like `sleep`)
    // can't be cancelled mid-wait. The redirect path uses
    // WaitForSingleObject(timeout) directly.
    auto dir = tempdir_for("timeout");
    auto log = dir / "run.log";

    crux::proc::RunOptions opts;
    opts.timeout_ms = 500;
    opts.redirect_output_to = log.string();

    auto t0 = std::chrono::steady_clock::now();
    crux::proc::RunResult r = crux::proc::run(
        cmake_bin(), {"-E", "sleep", "5"}, opts);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    CHECK(r.timed_out == true);
    // Should have returned in well under the 5s the child asked for.
    CHECK(elapsed < 3000);
    best_effort_remove_all(dir);
}
