#include "agent_control/log_sink.h"

#include <QString>
#include <QtGlobal>

#include <spdlog/spdlog.h>

namespace agent_control
{

namespace
{

QtMessageHandler g_previous_qt_handler = nullptr;

// Tracked separately from the handler pointer because qInstallMessageHandler
// returns nullptr when the default handler was in use. Keying "already
// installed" off the pointer would therefore re-install on a second call, and
// our handler would end up chained to itself -- infinite recursion on the first
// Qt warning.
bool g_qt_handler_installed = false;

// Qt's own diagnostics, funnelled into spdlog so they land in the same queryable
// stream as everything else instead of disappearing to stderr.
void qtMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    auto logger = spdlog::default_logger();
    const std::string text = message.toStdString();

    spdlog::source_loc loc{};
    if (context.file != nullptr)
    {
        loc = spdlog::source_loc{context.file, context.line,
                                 context.function != nullptr ? context.function : ""};
    }

    switch (type)
    {
        case QtDebugMsg:
            logger->log(loc, spdlog::level::debug, "[qt] {}", text);
            break;
        case QtInfoMsg:
            logger->log(loc, spdlog::level::info, "[qt] {}", text);
            break;
        case QtWarningMsg:
            logger->log(loc, spdlog::level::warn, "[qt] {}", text);
            break;
        case QtCriticalMsg:
            logger->log(loc, spdlog::level::err, "[qt] {}", text);
            break;
        case QtFatalMsg:
            logger->log(loc, spdlog::level::critical, "[qt] {}", text);
            break;
    }

    // Chain to whatever was installed before, so the message still reaches
    // stderr and a fatal one still aborts -- Qt relies on the handler returning
    // for QtFatalMsg to terminate.
    if (g_previous_qt_handler != nullptr)
    {
        g_previous_qt_handler(type, context, message);
    }
}

}  // namespace

std::shared_ptr<RingSink> installLogCapture(std::size_t capacity)
{
    auto ring = installLogRing(capacity);
    if (!g_qt_handler_installed)
    {
        g_previous_qt_handler = qInstallMessageHandler(qtMessageHandler);
        g_qt_handler_installed = true;
    }
    return ring;
}

}  // namespace agent_control
