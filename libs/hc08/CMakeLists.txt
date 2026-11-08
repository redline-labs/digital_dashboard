# The Freescale CPU08 (HC08) core: the instruction set, the machine that runs
# it with its devices and interrupts, S-record images, a disassembler and a
# small assembler, and three engines with one contract -- an interpreter, a
# decode-caching block engine, and a JIT that emits AArch64 or x86-64 through
# asmjit. The JIT is the default; tools/engine_diff holds the other two to the
# interpreter.
add_library(hc08 STATIC
    src/assembler.cpp
    src/disasm.cpp
    src/image.cpp
    src/isa.cpp
    src/machine.cpp
    src/jit/assembler_aarch64.cpp
    src/jit/assembler_x86_64.cpp
    src/jit/code_memory.cpp
    src/jit/host_probe.cpp
    src/jit/jit_cache.cpp
    src/jit/translator.cpp
)
target_include_directories(hc08 PUBLIC include)
target_link_libraries(hc08
    PUBLIC
        emu
        spdlog::spdlog
    PRIVATE
        asmjit::asmjit
)

# ---------------------------------------------------------------------- tests

# The ISA, the three engines, the machine, images and the JIT, without any
# firmware: hand-assembled programs and a flat memory map.
add_executable(hc08_test_core
    tests/test_cpu.cpp
    tests/test_engines.cpp
    tests/test_machine.cpp
    tests/test_image.cpp
    tests/test_jit_probe.cpp
    tests/test_jit.cpp
)
target_link_libraries(hc08_test_core PRIVATE hc08 mctest)
add_project_test(TARGET hc08_test_core LABELS hc08 unit)

# Random instruction sequences on two engines, compared after every step. The
# third run is an engine broken on purpose, which must be caught: a checker that
# cannot fail proves nothing.
add_executable(hc08_engine_diff tools/engine_diff.cpp)
target_link_libraries(hc08_engine_diff PRIVATE hc08)
add_project_test(TARGET hc08_engine_diff NAME hc08_test_engine_diff_block LABELS hc08 slow
    ARGS --engine-diff --engine block --cases 20000)
add_project_test(TARGET hc08_engine_diff NAME hc08_test_engine_diff_jit LABELS hc08 slow
    ARGS --engine-diff --engine jit --cases 20000)
add_project_test(TARGET hc08_engine_diff NAME hc08_test_engine_diff_catches_broken LABELS hc08 slow
    ARGS --engine-diff --engine broken --cases 2000 WILL_FAIL)
