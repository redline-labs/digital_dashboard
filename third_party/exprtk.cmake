# Fetch exprtk
#
# Repinned 2026-08-04 from 66883f0ddb034371ef38f2799f772c05bc904571, which
# upstream made unreachable: it is not on any ref any more, so FetchContent's
# clone-then-checkout fails with "fatal: unable to read tree" and a clean
# `rm -rf build && cmake -B build` could not configure at all. (The object was
# still fetchable by explicit SHA, which is why existing build trees kept
# working and the breakage only showed up on a from-scratch build.)
#
# This is master@1e4a80b5, the head at the time of repinning.
set(EXPRTK_GIT_TAG 1e4a80b5ec9b4832ed59c6faa65f625a01b18ef0)

FetchContent_Declare(
    exprtk
    GIT_REPOSITORY https://github.com/ArashPartow/exprtk.git
    GIT_TAG ${EXPRTK_GIT_TAG}
)

# Configure exprtk options
# exprtk is header-only, so no specific build options needed

FetchContent_MakeAvailable(exprtk)

# Create an interface library target for exprtk
add_library(exprtk_lib INTERFACE)
target_include_directories(exprtk_lib SYSTEM INTERFACE "${exprtk_SOURCE_DIR}")

# Add a namespaced alias for the exprtk target
add_library(exprtk::exprtk ALIAS exprtk_lib)

# Copy exprtk license
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/licenses/exprtk)
file(COPY ${exprtk_SOURCE_DIR}/license.txt ${exprtk_SOURCE_DIR}/readme.txt
     DESTINATION ${CMAKE_BINARY_DIR}/licenses/exprtk)

# Write version info. The tag comes from the variable so it cannot drift from
# what is actually fetched; the previous literal was a second copy of the SHA.
file(WRITE ${CMAKE_BINARY_DIR}/licenses/exprtk/fetch_info.txt
"Library: exprtk
Repository: https://github.com/ArashPartow/exprtk.git
Tag/Version: ${EXPRTK_GIT_TAG}
Shallow Clone: FALSE
Patches Applied: None
")
