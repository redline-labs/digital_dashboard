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
