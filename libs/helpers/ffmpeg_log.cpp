// SPDX-License-Identifier: GPL-3.0-or-later

#include "helpers/ffmpeg_log.h"

#include <spdlog/spdlog.h>

extern "C"
{
#include <libavutil/log.h>
}

#include <mutex>
#include <string>

namespace helpers
{
namespace
{

std::mutex g_mutex;
// ffmpeg emits a line in pieces, so partial output is accumulated until the
// newline that ends it. Guarded because decoder threads log too.
std::string g_partial;
int g_print_prefix = 1;

void forward(void* context, int level, const char* format, va_list args)
{
    if (level > av_log_get_level())
    {
        return;
    }

    char line[1024];
    std::lock_guard<std::mutex> lock(g_mutex);
    if (av_log_format_line2(context, level, format, args, line, sizeof(line), &g_print_prefix) < 0)
    {
        return;
    }
    g_partial += line;

    // Emit whole lines only; ffmpeg's progress output relies on being able to
    // build one up across several calls.
    size_t newline = g_partial.find('\n');
    while (newline != std::string::npos)
    {
        std::string message = g_partial.substr(0, newline);
        g_partial.erase(0, newline + 1);
        while (!message.empty() && (message.back() == '\r' || message.back() == ' '))
        {
            message.pop_back();
        }
        if (!message.empty())
        {
            if (level <= AV_LOG_ERROR)
            {
                SPDLOG_ERROR("[ffmpeg] {}", message);
            }
            else if (level <= AV_LOG_WARNING)
            {
                SPDLOG_WARN("[ffmpeg] {}", message);
            }
            else
            {
                // AV_LOG_INFO and below. This is where codecs put their
                // per-run statistics, which is detail rather than news.
                SPDLOG_DEBUG("[ffmpeg] {}", message);
            }
        }
        newline = g_partial.find('\n');
    }
}

}  // namespace

void routeFfmpegLogsToSpdlog()
{
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_callback(forward); });
}

}  // namespace helpers
