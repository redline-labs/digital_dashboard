// The JIT toolchain: asmjit assembles a host function into W^X code memory and it runs.
#include "hc08/jit/host_probe.h"
#include "mctest/test.h"

TEST(jit_probe_runs_generated_code) {
    if (sim::jit::host_architecture() == "none") {
        mctest::skip("no JIT assembler for this host");
        return;
    }
    CHECK_EQ(sim::jit::run_add_probe(40, 2), 42);
}
