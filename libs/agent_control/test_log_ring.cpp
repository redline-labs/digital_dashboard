// The queryable log ring behind app.logs.
//
// The properties that matter are the ones a polling caller depends on: seq is
// monotonic and never reused, since_seq returns only what is new, eviction is
// reported rather than silent, and Qt's own diagnostics actually arrive.

#include "agent_control/log_sink.h"

#include <QCoreApplication>
#include <QLoggingCategory>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/null_sink.h>

#include <cstdio>
#include <string>

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // Keep the test's own output clean: the ring is the only sink under test,
    // and a console sink would spray the deliberate warnings below into stderr
    // where they look like failures.
    spdlog::default_logger()->sinks().clear();
    spdlog::default_logger()->sinks().push_back(std::make_shared<spdlog::sinks::null_sink_mt>());
    spdlog::set_level(spdlog::level::trace);

    auto ring = agent_control::installLogCapture(/*capacity=*/16);
    check(ring != nullptr, "installLogCapture returns a ring");
    check(agent_control::logRing() == ring, "logRing() returns the installed ring");

    // ------------------------------------------------------------ basic capture
    {
        SPDLOG_INFO("hello ring");
        const auto r = ring->query({});
        check(!r.records.empty(), "a logged message reaches the ring");
        check(r.records.back().message.find("hello ring") != std::string::npos,
              "the message text is captured");
        check(r.records.back().line > 0, "source line is captured");
        check(!r.records.back().file.empty(), "source file is captured");
    }

    // ------------------------------------------------------------- seq cursor
    {
        const auto before = ring->query({});
        const std::uint64_t cursor = before.next_seq;

        // Polling with the previous next_seq must return nothing when nothing
        // has happened -- this is what makes incremental polling cheap.
        agent_control::RingSink::Query q;
        q.since_seq = cursor;
        check(ring->query(q).records.empty(), "since_seq with no new records returns nothing");

        SPDLOG_WARN("after cursor");
        const auto after = ring->query(q);
        check(after.records.size() == 1, "since_seq returns exactly the new record");
        // Guarded: a failed size check must not turn into a front() on an empty
        // vector, or the suite crashes instead of reporting which check failed.
        check(!after.records.empty() && after.records.front().seq == cursor,
              "the new record's seq continues the sequence");
        check(after.next_seq == cursor + 1, "next_seq advances by one");
    }

    // ----------------------------------------------------------- level filter
    {
        agent_control::RingSink::Query q;
        q.since_seq = ring->query({}).next_seq - 1;
        q.min_level = static_cast<int>(spdlog::level::err);

        SPDLOG_DEBUG("quiet");
        SPDLOG_ERROR("loud");

        const auto r = ring->query(q);
        bool saw_quiet = false;
        bool saw_loud = false;
        for (const auto& rec : r.records)
        {
            saw_quiet |= rec.message.find("quiet") != std::string::npos;
            saw_loud |= rec.message.find("loud") != std::string::npos;
        }
        check(!saw_quiet, "a level filter excludes lower-severity records");
        check(saw_loud, "a level filter keeps matching records");
    }

    // ------------------------------------------------------------ grep filter
    {
        SPDLOG_INFO("needle in here");
        agent_control::RingSink::Query q;
        q.grep = "NEEDLE";
        const auto r = ring->query(q);
        check(!r.records.empty(), "grep matches case-insensitively");

        q.grep = "definitely-not-present";
        check(ring->query(q).records.empty(), "grep excludes non-matching records");
    }

    // --------------------------------------------------------------- eviction
    {
        // Ring holds 16. Overflow it and confirm the loss is reported rather
        // than silently swallowed -- a caller that thinks it has read everything
        // when it has not would draw wrong conclusions from the gap.
        const std::uint64_t cursor = ring->query({}).next_seq;
        for (int i = 0; i < 40; ++i)
        {
            SPDLOG_INFO("flood {}", i);
        }

        agent_control::RingSink::Query q;
        q.since_seq = cursor;
        const auto r = ring->query(q);
        check(r.total_held <= 16, "the ring does not grow past its capacity");
        check(r.dropped > 0, "eviction past the caller's cursor is reported");
    }

    // ------------------------------------------------------------------ limit
    {
        agent_control::RingSink::Query q;
        q.limit = 3;
        const auto r = ring->query(q);
        check(r.records.size() == 3, "limit caps the record count");

        // Newest-wins: a caller catching up after a burst wants the tail.
        const auto all = ring->query({});
        check(!r.records.empty() && !all.records.empty() &&
                  r.records.back().seq == all.records.back().seq,
              "limit keeps the newest records, not the oldest");
    }

    // ---------------------------------------------------- seq is never reused
    {
        const auto r = ring->query({});
        std::uint64_t previous = 0;
        bool monotonic = true;
        for (const auto& rec : r.records)
        {
            monotonic &= rec.seq > previous;
            previous = rec.seq;
        }
        check(monotonic, "seq is strictly increasing across the whole ring");
    }

    // ------------------------------------------------------------- Qt bridge
    {
        // Qt's diagnostics go to stderr and vanish otherwise; nothing else in
        // this codebase captures them, and a QPA complaint is often the
        // explanation for a screenshot that came back wrong.
        const std::uint64_t cursor = ring->query({}).next_seq;

        qWarning("qt bridge test message");

        agent_control::RingSink::Query q;
        q.since_seq = cursor;
        const auto r = ring->query(q);

        bool found = false;
        for (const auto& rec : r.records)
        {
            if (rec.message.find("qt bridge test message") != std::string::npos)
            {
                found = true;
                check(rec.message.find("[qt]") != std::string::npos,
                      "a Qt message is tagged so its origin is obvious");
                check(rec.level == static_cast<int>(spdlog::level::warn),
                      "qWarning maps to the warn level");
            }
        }
        check(found, "qWarning reaches the ring through the message handler");
    }

    std::printf("%s: %d checks, %d failures\n", argv[0], g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
