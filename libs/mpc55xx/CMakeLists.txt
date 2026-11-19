# The MPC55xx around the e200 core: the MPC5554's memory map, the boot ROM's
# work at reset, the INTC, FlexCAN A-C on emu's CAN bus, eSCI A as a console,
# and storage stubs (model gaps) for the modules not modelled.
add_library(mpc55xx STATIC
    src/flexcan.cpp
    src/intc.cpp
    src/registers.cpp
    src/soc.cpp
)
target_include_directories(mpc55xx PUBLIC include)
target_link_libraries(mpc55xx PUBLIC e200 helpers)

# The modules and a boot, from bare images assembled in the tests.
add_executable(mpc55xx_test_soc tests/test_soc.cpp)
target_link_libraries(mpc55xx_test_soc PRIVATE mpc55xx mctest)
add_project_test(TARGET mpc55xx_test_soc LABELS mpc55xx unit)
