// The JIT's toolchain probe: assembles a tiny host function with asmjit into
// CodeMemory and calls it, so a broken asmjit, assembler or W^X code memory on a new host fails one small test before
// the translator's tests.
#pragma once

#include <cstdint>
#include <string>

namespace sim::jit {

// "aarch64" / "x86_64" / "none" (no assembler for this host).
std::string host_architecture();

// Assembles `uint64_t f(uint64_t x) { return x + addend; }`, runs it on `x` and returns the result. Throws JitError when
// the host has no assembler or the code memory cannot be set up.
uint64_t run_add_probe(uint64_t x, uint32_t addend);

}  // namespace sim::jit
