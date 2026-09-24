# Geophysical models the nodes load at run time, kept verbatim as their
# publishers distribute them and stored in Git LFS (see README.md). They ship
# the way the web console's assets do: installed under the prefix beside bin/,
# and found by core::paths::resource("models/...") -- which also finds them
# here, in the checkout, for a developer build.
#
# THE ONE WAY THIS GOES WRONG QUIETLY. A checkout that never ran `git lfs pull`
# has ~130-byte pointer files where the models should be. Installing those
# would put a file on the board that every loader refuses, and the car would
# run on normal gravity with only a health check saying so. So a pointer is
# caught here: a developer build warns and installs nothing from models/, and
# a build that sets REDLINE_REQUIRE_MODELS (the image does) stops.

option(REDLINE_REQUIRE_MODELS "Fail the configure if the LFS-stored models are not checked out" OFF)

set(_redline_models
    deflec2022/SDEFLEC2022.NA.eta.beta_v0a.bin
    deflec2022/SDEFLEC2022.NA.xi.beta_v0a.bin
)
set(_missing "")
foreach(_model IN LISTS _redline_models)
    set(_path "${CMAKE_CURRENT_SOURCE_DIR}/${_model}")
    if(NOT EXISTS "${_path}")
        list(APPEND _missing "${_model} (absent)")
        continue()
    endif()
    file(SIZE "${_path}" _size)
    if(_size LESS 1024)
        list(APPEND _missing "${_model} (a Git LFS pointer)")
    endif()
endforeach()

if(_missing)
    list(JOIN _missing "\n    " _list)
    set(_why "models/ is not checked out:\n    ${_list}\nRun `git lfs install && git lfs pull` in the checkout.")
    if(REDLINE_REQUIRE_MODELS)
        message(FATAL_ERROR "${_why}")
    endif()
    message(WARNING "${_why}\nNothing from models/ will be installed; the state estimator will run on normal gravity.")
    return()
endif()

redline_install_data(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/deflec2022" DESTINATION models/deflec2022 COMPONENT nodes)
