# The MoTeC M150 (an M1 "ecu120"), emulated: its MPC55xx on an e200 core, the
# board and what is wired to its connectors: the board (the MPC5554, external
# RAM, the topology), `m150_sim run` and `m150_sim serve` (docs/sims/m150.md).
#
# No target code is in the tree: the image it runs -- a kernel and an M1
# application -- is built and kept outside it, like the PDM's firmware.
#
# Layers, each including only those below it (sims/tools/check_layers.py):
#   board/   the M150 board; board/topology/ is its I/O, connectors, driver
#            devices and hardware functions (the source of truth, maintained
#            here; tests/test_topology.cpp keeps it consistent)
#   plant/   what is wired to the connectors
#   top/     the whole system: instance, session, the agent methods, serve

add_library(m150_topology STATIC
    board/topology/topology.cpp
    board/topology/m150.cpp
)
# Included as "m150/board/topology/topology.h".
target_include_directories(m150_topology PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_library(m150_board STATIC board/m150.cpp)
target_link_libraries(m150_board PUBLIC m150_topology mpc55xx)

add_library(m150_top STATIC top/candump.cpp top/served.cpp top/target.cpp)
target_link_libraries(m150_top PUBLIC m150_board sim_host)

add_executable(m150_sim main.cpp verbs.cpp)
target_link_libraries(m150_sim PRIVATE m150_top core cli node_health zenoh_pub_sub)

add_executable(m150_test_topology tests/test_topology.cpp)
target_link_libraries(m150_test_topology PRIVATE m150_topology mctest)
add_project_test(TARGET m150_test_topology LABELS m150 unit)

# An image built outside the tree (a kernel and an M1 application: the research
# tree's emu/m150_mvp builds one). The boot test skips without it.
set(M150_IMAGE "" CACHE FILEPATH "a raw M150 internal-flash image (kernel and application)")
add_executable(m150_test_boot tests/test_boot.cpp)
target_link_libraries(m150_test_boot PRIVATE m150_top mctest)
add_project_test(TARGET m150_test_boot LABELS m150 unit)
set_tests_properties(m150_test_boot PROPERTIES ENVIRONMENT "M150_IMAGE=${M150_IMAGE}")

# The shared debugger on the M150 through its agent methods: breakpoints,
# registers, disassembly, backtrace, step/over/finish, watches, snapshots.
# Skips without M150_IMAGE and M150_SYMBOLS (the image's ELF files).
set(M150_SYMBOLS "" CACHE STRING "the M150 image's ELF files, comma-separated (kernel, application)")
add_executable(m150_test_debug tests/test_debug.cpp)
target_link_libraries(m150_test_debug PRIVATE m150_top mctest)
add_project_test(TARGET m150_test_debug LABELS m150 unit)
set_tests_properties(m150_test_debug PROPERTIES ENVIRONMENT "M150_IMAGE=${M150_IMAGE};M150_SYMBOLS=${M150_SYMBOLS}")

# serve in-process over zenoh: the application's frames on CAN1's rx topic,
# frames from its tx topic into the application, the methods. Skips without
# M150_IMAGE or a zenoh session.
add_executable(m150_test_serve tests/test_serve.cpp)
target_link_libraries(m150_test_serve PRIVATE m150_top zenoh_pub_sub schemas capnp spdlog::spdlog)
add_project_test(TARGET m150_test_serve LABELS m150 net slow ENVIRONMENT "M150_IMAGE=${M150_IMAGE}")

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME m150_test_layers
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/sims/tools/check_layers.py ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(m150_test_layers PROPERTIES LABELS "m150;unit")
endif()
