#include "core/core.h"

#include <spdlog/spdlog.h>

namespace core
{

void setupLogging(bool debug_enabled)
{
    // A rotating file sink belongs here too -- scope/main.cpp and
    // dashboard/dashboard/main.cpp each build their own, which is why
    // logs/rotating.txt and logs/scope.txt exist with different rotation
    // policies. Left commented rather than deleted because that consolidation is
    // a separate change with its own decision to make about where a *tool*
    // should write, and a tool writing into the repo's logs/ by default would be
    // a surprise.
    //
    //size_t max_size_bytes = 5u * 1024u * 1024u;  // 5MB
    //size_t max_files = 3u;
    //auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>("logs/rotating.txt", max_size_bytes, max_files, true);
    //spdlog::default_logger()->sinks().push_back(file_sink);

    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");
    spdlog::set_level(debug_enabled ? spdlog::level::debug : spdlog::level::info);
}

}  // namespace core
