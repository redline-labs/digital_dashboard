cmake_minimum_required(VERSION 3.10)

project(fake_service)

# A responder for developing the web console's service-call path without
# hardware. See the source; same category as tools/rauc_stub, and like it, never
# shipped -- no redline_install().
add_executable(fake_service
    fake_service.cpp
)

target_link_libraries(fake_service PRIVATE
    core
    spdlog::spdlog
    cxxopts::cxxopts
    zenoh_pub_sub
    schemas
    capnp
)
