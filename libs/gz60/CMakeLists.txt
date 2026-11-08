# The MC68HC908GZ60's on-chip modules around a CPU08 core: SIM (system
# integration), CGM (clock generator), ports, COP, the two timers and the
# timebase, the ADC, flash, and MSCAN08 -- the CAN controller -- composed into
# one SoC. A board model (sims/pdm32) wires its pins to the world.
add_library(gz60 STATIC
    src/adc.cpp
    src/cgm.cpp
    src/cop.cpp
    src/flash.cpp
    src/mscan.cpp
    src/ports.cpp
    src/sim_module.cpp
    src/soc.cpp
    src/stub.cpp
    src/tim.cpp
    src/timebase.cpp
)
target_include_directories(gz60 PUBLIC include)
target_link_libraries(gz60 PUBLIC hc08 can)

# The modules register by register, and MSCAN's acceptance filter, buffers and
# frame timing against a bus.
add_executable(gz60_test_modules
    tests/test_mcu.cpp
    tests/test_mscan.cpp
)
target_link_libraries(gz60_test_modules PRIVATE gz60 mctest)
add_project_test(TARGET gz60_test_modules LABELS gz60 unit)
