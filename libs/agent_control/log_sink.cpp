#include "agent_control/log_sink.h"

#include <QString>
#include <QtGlobal>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <utility>

namespace agent_control
{

namespace
{

std::shared_ptr<RingSink> g_ring;
QtMessageHandler g_previous_qt_handler = nullptr;

// Tracked separately from the handler pointer because qInstallMessageHandler
// returns nullptr when the default handler was in use. Keying "already
// installed" off the pointer would therefore re-install on a second call, and
// our handler would end up chained to itself -- infinite recursion on the first
// Qt warning.
bool g_qt_handler_installed = false;

bool containsCaseInsensitive(const std::string& haystack, const std::string& needle)
{
    if (needle.empty())
    {
        return true;
    }
    const auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                                [](unsigned char a, unsigned char b)
                                { return std::tolower(a) == std::tolower(b); });
    return it != haystack.end();
}

std::string threadIdString(std::size_t id)
{
    std::ostringstream out;
    out << id;
    return out.str();
}

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

json LogRecord::toJson() const
{
    json out = json::object();
    out["seq"] = seq;
    out["t_us"] = timestamp_us;
    out["level"] = level_name;
    out["logger"] = logger;
    out["thread"] = thread;
    out["message"] = message;
    if (!file.empty())
    {
        out["file"] = file;
        out["line"] = line;
    }
    if (!func.empty())
    {
        out["func"] = func;
    }
    return out;
}

RingSink::RingSink(std::size_t capacity) : capacity_(capacity == 0 ? 1 : capacity)
{
}

void RingSink::sink_it_(const spdlog::details::log_msg& msg)
{
    LogRecord record;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        record.seq = next_seq_++;
    }

    record.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              msg.time.time_since_epoch())
                              .count();
    record.level = static_cast<int>(msg.level);
    record.level_name = std::string(spdlog::level::to_string_view(msg.level).data(),
                                    spdlog::level::to_string_view(msg.level).size());
    record.logger = std::string(msg.logger_name.data(), msg.logger_name.size());
    record.thread = threadIdString(msg.thread_id);
    record.message = std::string(msg.payload.data(), msg.payload.size());

    if (msg.source.filename != nullptr)
    {
        record.file = msg.source.filename;
        record.line = msg.source.line;
    }
    if (msg.source.funcname != nullptr)
    {
        record.func = msg.source.funcname;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    records_.push_back(std::move(record));
    while (records_.size() > capacity_)
    {
        records_.pop_front();
        ++evicted_;
    }
}

void RingSink::flush_()
{
    // Nothing to flush: the ring is the storage.
}

RingSink::Result RingSink::query(const Query& q) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    Result result;
    result.total_held = records_.size();
    result.next_seq = next_seq_;

    // How much the caller missed: records between where they were reading and
    // the oldest one still held. A caller starting from 0 has missed everything
    // already evicted.
    if (!records_.empty())
    {
        const std::uint64_t want_from = std::max<std::uint64_t>(q.since_seq, 1);
        if (want_from < records_.front().seq)
        {
            result.dropped = records_.front().seq - want_from;
        }
    }

    for (const LogRecord& record : records_)
    {
        // Inclusive lower bound: `next_seq` names the seq the next record WILL
        // get, so feeding it straight back must return that record once it
        // exists. An exclusive bound here would skip exactly one record per poll
        // -- a slow, silent leak of log lines that only shows up as a confusing
        // gap much later.
        if (record.seq < q.since_seq)
        {
            continue;
        }
        if (record.level < q.min_level)
        {
            continue;
        }
        if (!q.logger.empty() && record.logger != q.logger)
        {
            continue;
        }
        if (!containsCaseInsensitive(record.message, q.grep))
        {
            continue;
        }
        result.records.push_back(record);
    }

    // Keep the newest when the window overflows: a caller catching up after a
    // burst wants the tail, not the start of the backlog.
    if (q.limit > 0 && result.records.size() > q.limit)
    {
        result.records.erase(result.records.begin(),
                             result.records.end() - static_cast<std::ptrdiff_t>(q.limit));
    }

    return result;
}

std::shared_ptr<RingSink> installLogCapture(std::size_t capacity)
{
    if (g_ring == nullptr)
    {
        g_ring = std::make_shared<RingSink>(capacity);
        spdlog::default_logger()->sinks().push_back(g_ring);

        // The sink filters by level itself, so let everything through to it and
        // let app.logs decide. Without this the default logger's own level would
        // silently drop debug records before the ring ever saw them.
        g_ring->set_level(spdlog::level::trace);
    }

    if (!g_qt_handler_installed)
    {
        g_previous_qt_handler = qInstallMessageHandler(qtMessageHandler);
        g_qt_handler_installed = true;
    }

    return g_ring;
}

std::shared_ptr<RingSink> logRing()
{
    return g_ring;
}

}  // namespace agent_control
