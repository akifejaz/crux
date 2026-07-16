#include "cli.h"
#include "pipeline.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <cstdio>
#include <exception>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    try {
        auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        auto lg = std::make_shared<spdlog::logger>("crux", sink);
        spdlog::set_default_logger(lg);
        spdlog::set_level(spdlog::level::info);
        spdlog::set_pattern("%^%L%$ %v");
    } catch (...) {
        // logging is best-effort
    }

    auto r = crux::parse_cli(argc, argv);
    if (!r.should_run) return r.exit_code;
    int code = 1;
    try {
        code = crux::run_pipeline(r.config);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
        code = 1;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    return code;
}
