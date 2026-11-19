# The Freescale/NXP e200 core as the MPC55xx carry it (e200z6: 32-bit Book E,
# isel, the MAS-register MMU, the SPE's scalar single precision): the
# instruction set, the interrupts and the MMU, a disassembler and a small
# assembler for programs written in C++, and the machine that runs it with its
# devices on a 32-bit physical bus. The engines share one contract with the
# HC08's: an interpreter, a decode-caching block engine and a JIT.
add_library(e200 STATIC
    src/isa.cpp
    src/machine.cpp
    src/spe.cpp
    src/jit/assembler_aarch64.cpp
    src/jit/translator.cpp
)
target_include_directories(e200 PUBLIC include)
target_link_libraries(e200
    PUBLIC
        emu
        emu_jit
        spdlog::spdlog
    PRIVATE
        asmjit::asmjit
)

# ---------------------------------------------------------------------- tests

# The ISA, interrupts, MMU and SPE against a flat memory, from programs
# assembled in the tests.
add_executable(e200_test_core
    tests/test_cpu.cpp
    tests/test_machine.cpp
)
target_link_libraries(e200_test_core PRIVATE e200 mctest)
add_project_test(TARGET e200_test_core LABELS e200 unit)

# Random programs on the interpreter and another engine, compared at random
# points. The third run is the JIT with a register corrupted on purpose, which
# must be caught: a checker that cannot fail proves nothing.
add_executable(e200_engine_diff tools/engine_diff.cpp)
target_link_libraries(e200_engine_diff PRIVATE e200)
add_project_test(TARGET e200_engine_diff NAME e200_test_engine_diff_block LABELS e200 slow
    ARGS --engine block --cases 1000)
add_project_test(TARGET e200_engine_diff NAME e200_test_engine_diff_jit LABELS e200 slow
    ARGS --engine jit --cases 1000)
add_project_test(TARGET e200_engine_diff NAME e200_test_engine_diff_catches_broken LABELS e200 slow
    ARGS --engine broken --cases 50 WILL_FAIL)
