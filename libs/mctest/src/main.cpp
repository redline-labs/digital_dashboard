// The runner every mctest program shares.
#include "mctest/test.h"

#include <spdlog/spdlog.h>

int main(int argc, char** argv) {
    // Warnings and up: a test's own output is its checks.
    spdlog::set_level(spdlog::level::warn);
    return mctest::run(argc, argv);
}
