cmake_minimum_required(VERSION 3.10)

project(plist)

# Apple property lists, in both wire formats this project meets:
#
#   * binary ("bplist00") -- CarPlay's RTSP-style control channel, which is where
#     this code started out (it lived in libs/airplay).
#   * XML -- the usbmux socket and the lockdown handshake.
#
# One Value model serves both, which is the point of having it here rather than
# inside either consumer.
add_library(plist STATIC
    binary.cpp
    xml.cpp
)

target_include_directories(plist PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(plist PRIVATE
    spdlog::spdlog
)

# Round-trip and known-answer tests, runnable without hardware.
foreach(test binary xml)
    add_executable(plist_test_${test} test_${test}.cpp)
    target_link_libraries(plist_test_${test} PRIVATE
        plist
        spdlog::spdlog
    )
endforeach()

# Differential test against libplist, the implementation on the other end of
# every usbmux and lockdown socket we talk to. Built only while that dependency
# is still vendored -- it is the evidence for each step that removes a piece of
# it, and it goes away with the last one.
if(TARGET vendored_plist)
    add_executable(plist_test_libplist_conformance test_libplist_conformance.cpp)
    target_link_libraries(plist_test_libplist_conformance PRIVATE
        plist
        vendored_plist
        spdlog::spdlog
    )
endif()
