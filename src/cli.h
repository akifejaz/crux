#pragma once

#include "config.h"

namespace crux {

struct CliResult {
    int exit_code = 0;   // if non-zero, main exits with this (e.g. --help handled)
    bool should_run = false;
    Config config{};
};

// Parses argv. On --help/--version, prints and returns should_run=false.
CliResult parse_cli(int argc, char** argv);

// Semantic version of the crux binary (compile-time).
const char* version_string();

} // namespace crux
