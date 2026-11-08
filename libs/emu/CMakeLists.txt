# What every emulator is wired from: simulated time (Clock), the electrical net
# between parts (Net, Part, IrqLine), a CAN bus between simulated controllers
# (CanBus, CanNode, Tap), snapshot I/O, and the record of where a model is known
# to be incomplete (model_gap). No CPU and no device in particular -- libs/hc08
# and libs/gz60 sit on it, and sims/ assembles a whole product from them.
add_library(emu INTERFACE)
target_include_directories(emu INTERFACE include)
target_link_libraries(emu INTERFACE helpers spdlog::spdlog)
