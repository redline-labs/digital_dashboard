// The CPU engines and their contract: every engine takes (CpuState&, Env&), runs to
// the environment's cycle limit or a stop, and must give identical steps, stops and bus accesses. They differ only in
// where the decoded instruction comes from:
//
//   Interpreter  decodes each instruction as it runs it (hc08/cpu.h interpret)
//   Block        runs pre-decoded instructions from the decode cache (hc08/block_engine.h); the cache is dropped
//                page by page when code memory is written (flash programming, code in RAM)
//   Jit          runs the decode cache's blocks as host code, translated by asmjit on first use (libs/hc08/src/jit/); while
//                a pc or data watch is set (or on a host without an assembler) the machine runs the Block engine
//
// hc08_engine_diff --engine-diff runs random instruction sequences on two engines and compares them after every step.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace sim {

enum class CpuEngine : uint8_t { Interpreter, Block, Jit };

#if defined(__aarch64__) || defined(__x86_64__)
inline constexpr CpuEngine kDefaultCpuEngine = CpuEngine::Jit;
#else
inline constexpr CpuEngine kDefaultCpuEngine = CpuEngine::Block;
#endif

inline const char* engine_name(CpuEngine engine) {
    switch (engine) {
        case CpuEngine::Interpreter:
            return "interpreter";
        case CpuEngine::Block:
            return "block";
        case CpuEngine::Jit:
            return "jit";
    }
    return "?";
}

// "interp" / "interpreter", "block", "jit"
inline std::optional<CpuEngine> parse_engine(std::string_view text) {
    if (text == "interp" || text == "interpreter") {
        return CpuEngine::Interpreter;
    }
    if (text == "block") {
        return CpuEngine::Block;
    }
    if (text == "jit") {
        return CpuEngine::Jit;
    }
    return std::nullopt;
}

}  // namespace sim
