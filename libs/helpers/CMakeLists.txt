add_library(helpers INTERFACE)
target_include_directories(helpers INTERFACE
    include
)
target_link_libraries(helpers INTERFACE
    yaml-cpp
)
# Routing ffmpeg's own logging into spdlog. A separate target from the
# header-only `helpers` above because it has to link libavutil, and almost
# nothing that uses helpers has anything to do with ffmpeg.
#
# Optional, because the REQUIRED probe below used to make libavutil a hard
# prerequisite of this whole directory -- including the interface target above,
# which is headers and yaml-cpp. ON by default; OFF is a consumer saying it has
# no ffmpeg, not a fallback we pick when the probe fails. Guessing there is how
# a build silently drops video logging.
option(HELPERS_WITH_FFMPEG "Build the ffmpeg-to-spdlog log bridge (needs libavutil)" ON)

if(HELPERS_WITH_FFMPEG)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(HELPERS_LIBAVUTIL REQUIRED IMPORTED_TARGET libavutil)

    add_library(helpers_ffmpeg STATIC ffmpeg_log.cpp)
    target_include_directories(helpers_ffmpeg PUBLIC include)
    target_link_libraries(helpers_ffmpeg
        PUBLIC
            PkgConfig::HELPERS_LIBAVUTIL
        PRIVATE
            spdlog::spdlog
    )
endif()
