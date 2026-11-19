# What every emulator is wired from: simulated time (Clock), the electrical net
# between parts (Net, Part, IrqLine), a CAN bus between simulated controllers
# (CanBus, CanNode, Tap), snapshot I/O, and the record of where a model is known
# to be incomplete (model_gap). No CPU and no device in particular -- libs/hc08,
# libs/e200 and libs/gz60 sit on it, and sims/ assembles a whole product from
# them.
add_library(emu INTERFACE)
target_include_directories(emu INTERFACE include)
target_link_libraries(emu INTERFACE helpers spdlog::spdlog)

add_executable(emu_test_timed tests/test_timed.cpp)
target_link_libraries(emu_test_timed PRIVATE emu mctest)
add_project_test(TARGET emu_test_timed LABELS emu unit)

# The JIT's host side, shared by every core's JIT: W^X code memory (MAP_JIT on
# macOS, a double mapping elsewhere), the error the JIT layer throws, and a probe
# that assembles and runs a tiny host function -- so a host whose JIT cannot work
# fails one small test before any translator's.
add_library(emu_jit STATIC
    src/jit/code_memory.cpp
    src/jit/host_probe.cpp
)
target_include_directories(emu_jit PUBLIC include)
target_link_libraries(emu_jit PUBLIC spdlog::spdlog PRIVATE asmjit::asmjit)

add_executable(emu_test_jit_probe tests/test_jit_probe.cpp)
target_link_libraries(emu_test_jit_probe PRIVATE emu_jit mctest)
add_project_test(TARGET emu_test_jit_probe LABELS emu unit)
