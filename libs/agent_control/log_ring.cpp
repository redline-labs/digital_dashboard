#include "agent_control/log_ring.h"

#include "agent_control/params.h"

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

std::shared_ptr<RingSink> installLogRing(std::size_t capacity)
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
    return g_ring;
}

std::shared_ptr<RingSink> logRing()
{
    return g_ring;
}

MethodResult queryLogs(const json& params)
{
    auto ring = logRing();
    if (ring == nullptr)
    {
        return std::unexpected(AgentError{
            ErrorCode::kUnsupportedInApp,
            "Log capture is not installed in this process.",
            json::object()});
    }

    RingSink::Query query;

    if (params.contains("since_seq"))
    {
        if (!params["since_seq"].is_number_unsigned())
        {
            return std::unexpected(
                badParams("'since_seq' must be a non-negative integer."));
        }
        query.since_seq = params["since_seq"].get<std::uint64_t>();
    }

    const auto level_name = optionalParam<std::string>(params, "level");
    if (!level_name.has_value())
    {
        return std::unexpected(level_name.error());
    }
    if (level_name->has_value() && !level_name->value().empty())
    {
        const auto level = spdlog::level::from_str(level_name->value());
        // from_str returns `off` for anything it does not recognise, so a
        // typo would otherwise silently filter out every record.
        if (level == spdlog::level::off && level_name->value() != "off")
        {
            return std::unexpected(badParams(
                "Unknown level '" + level_name->value() +
                "'; expected trace, debug, info, warn, err, critical or off."));
        }
        query.min_level = static_cast<int>(level);
    }

    const auto grep = optionalParam<std::string>(params, "grep");
    if (!grep.has_value())
    {
        return std::unexpected(grep.error());
    }
    query.grep = grep->value_or("");

    const auto logger = optionalParam<std::string>(params, "logger");
    if (!logger.has_value())
    {
        return std::unexpected(logger.error());
    }
    query.logger = logger->value_or("");

    const auto limit = optionalParam<int>(params, "limit");
    if (!limit.has_value())
    {
        return std::unexpected(limit.error());
    }
    const int limit_value = limit->value_or(200);
    if (limit_value < 0)
    {
        return std::unexpected(badParams("'limit' must not be negative."));
    }
    query.limit = static_cast<std::size_t>(limit_value);

    const auto result = ring->query(query);

    json records = json::array();
    for (const LogRecord& record : result.records)
    {
        records.push_back(record.toJson());
    }

    json out = json::object();
    out["records"] = std::move(records);
    // Feed this straight back as since_seq to get only what is new.
    out["next_seq"] = result.next_seq;
    out["held"] = result.total_held;
    if (result.dropped > 0)
    {
        out["dropped"] = result.dropped;
        out["dropped_note"] =
            "The ring wrapped: this many records were evicted before you read "
            "them. Poll more often or raise the ring capacity.";
    }
    return out;
}

}  // namespace agent_control
