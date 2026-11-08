#ifndef AGENT_CONTROL_LOG_RING_H_
#define AGENT_CONTROL_LOG_RING_H_

#include "agent_control/error.h"

#include <spdlog/sinks/base_sink.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace agent_control
{

struct LogRecord
{
    std::uint64_t seq = 0;   // Monotonic, never reused.
    std::int64_t timestamp_us = 0;
    int level = 0;           // spdlog::level::level_enum, as an int.
    std::string level_name;
    std::string logger;
    std::string thread;      // Which thread logged it: zenoh callbacks vs the GUI.
    std::string file;
    int line = 0;
    std::string func;
    std::string message;

    json toJson() const;
};

// An in-process log sink an agent can query over the control socket, instead of
// tailing a file or reading pipes.
//
// Not spdlog's own ringbuffer_sink: that stores pre-formatted strings and has no
// cursor, so a caller cannot ask "what is new since I last looked" and cannot
// filter on level or source without re-parsing text. The monotonic `seq` here is
// the whole point -- polling with since_seq returns only what has arrived.
class RingSink : public spdlog::sinks::base_sink<std::mutex>
{
  public:
    explicit RingSink(std::size_t capacity = 5000);

    struct Query
    {
        std::uint64_t since_seq = 0;
        int min_level = 0;            // spdlog level; 0 (trace) keeps everything.
        std::string grep;             // Case-insensitive substring of the message.
        std::string logger;           // Exact logger name, empty for any.
        std::size_t limit = 200;
    };

    struct Result
    {
        std::vector<LogRecord> records;
        std::uint64_t next_seq = 0;
        std::uint64_t dropped = 0;    // Records evicted before ever being read.
        std::size_t total_held = 0;
    };

    Result query(const Query& q) const;

  protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override;

  private:
    mutable std::mutex mutex_;
    std::deque<LogRecord> records_;
    std::size_t capacity_;
    std::uint64_t next_seq_ = 1;
    std::uint64_t evicted_ = 0;
};

// Installs the ring as an additional sink on spdlog's default logger, once.
// Qt-free; a GUI app calls installLogCapture() instead, which also routes Qt's
// own diagnostics into it.
std::shared_ptr<RingSink> installLogRing(std::size_t capacity = 5000);

// The installed ring, or nullptr if installLogCapture has not been called.
std::shared_ptr<RingSink> logRing();

// The app.logs method: reads since_seq, level, grep, logger and limit from
// `params` and queries the installed ring.
MethodResult queryLogs(const json& params);

}  // namespace agent_control

#endif  // AGENT_CONTROL_LOG_RING_H_
