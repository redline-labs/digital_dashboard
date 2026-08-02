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
# libplist_vectors carries documents captured from libplist while it was still a
# dependency; see the file header for why they are frozen rather than generated.
foreach(test binary xml libplist_vectors)
    add_executable(plist_test_${test} test_${test}.cpp)
    target_link_libraries(plist_test_${test} PRIVATE
        plist
        spdlog::spdlog
    )
    add_project_test(TARGET plist_test_${test} LABELS plist unit)
endforeach()
