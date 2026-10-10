// ScopeSession's transitions, without a bus or a window.
//
// Every rule here is a lifetime rule that used to be testable only by building
// a ScopeWindow: rebinding happens while the old source is alive, a review is
// destroyed before the recorder whose buffer it reads, a failed open changes
// nothing, and the save mark is a watermark rather than a latch.

#include "scope/empty_source.h"
#include "scope/recorded_source.h"
#include "scope/scope_recorder.h"
#include "scope/scope_session.h"

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// A live source that notes its own destruction in a shared log.
class LoggingLiveSource : public scope::EmptySource
{
  public:
    LoggingLiveSource(std::vector<std::string>& log, std::string name) :
        log_(log), name_(std::move(name))
    {
    }
    ~LoggingLiveSource() override { log_.push_back("destroyed " + name_); }

    scope::SourceCaps caps() const override
    {
        scope::SourceCaps caps;
        caps.live = true;
        return caps;
    }

  private:
    std::vector<std::string>& log_;
    std::string name_;
};

constexpr std::uint64_t kBase = 1'785'000'000'000'000'000;

bag::QueuedMessage message(std::uint64_t offset_ns)
{
    bag::QueuedMessage message;
    message.key = "vehicle/engine/rpm";
    message.schema = "EngineRpm";
    message.payload.assign(16, 0x42);
    message.log_time_ns = kBase + offset_ns;
    message.publish_time_ns = message.log_time_ns;
    return message;
}

struct Harness
{
    std::vector<std::string> log;
    int live_built = 0;
    std::size_t recorder_bytes = 0;
    double recorder_seconds = 0.0;
    // What a rebind hook wants to know about the session at that moment.
    std::function<void(scope::DataSource&)> on_rebind;
    std::unique_ptr<scope::ScopeSession> session;

    Harness()
    {
        scope::ScopeSession::Hooks hooks;
        hooks.rebind = [this](scope::DataSource& next)
        {
            log.push_back(next.caps().live ? "rebind live" : "rebind offline");
            if (on_rebind)
            {
                on_rebind(next);
            }
        };
        hooks.changed = [this]() { log.push_back("changed"); };

        scope::ScopeSession::Factories factories;
        factories.live = [this]() -> std::unique_ptr<scope::DataSource>
        {
            ++live_built;
            return std::make_unique<LoggingLiveSource>(log, "live" + std::to_string(live_built));
        };
        factories.recorder = [this](std::size_t max_bytes, double max_seconds)
        {
            recorder_bytes = max_bytes;
            recorder_seconds = max_seconds;
            return std::make_unique<scope::ScopeRecorder>(max_bytes, max_seconds,
                                                          scope::ScopeRecorder::Feed::kNothing);
        };
        session = std::make_unique<scope::ScopeSession>(std::move(hooks), std::move(factories));
    }
};

std::filesystem::path scratchDirectory(const std::string& name)
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("scope_test_session_" + name);
    std::filesystem::remove_all(path);
    return path;
}

void testStartsOfflineWithoutBuildingAnything()
{
    Harness h;
    expect(!h.session->isOnline(), "a new session is offline");
    expect(h.live_built == 0, "and built no live source: an offline window is not on the bus");
    expect(h.session->recorder() == nullptr, "and has no recorder");
    expect(h.session->label().empty(), "and names nothing");
    expect(h.log.empty(), "and rebound nothing");
}

void testRebindRunsWhileTheOldSourceIsAlive()
{
    Harness h;
    h.session->setSource(std::make_unique<LoggingLiveSource>(h.log, "first"));
    h.log.clear();
    h.session->setSource(std::make_unique<LoggingLiveSource>(h.log, "second"));
    expect(h.log == std::vector<std::string>{"rebind live", "destroyed first", "changed"},
           "rebind, THEN the old source is destroyed, THEN changed");

    h.log.clear();
    h.session->setSource(nullptr);
    expect(h.log.empty(), "a null source is ignored");
}

void testAFailedOpenChangesNothing()
{
    Harness h;
    const scope::DataSource* before = &h.session->source();
    const auto opened = h.session->openRecording(scratchDirectory("missing").string());
    expect(!opened, "a directory that is not a bag does not open");
    expect(&h.session->source() == before, "and the source is the one from before");
    expect(h.log.empty(), "and nothing was rebound");
    expect(h.session->label().empty(), "and the label is unchanged");
}

void testOnlineReviewAndBackAgain()
{
    Harness h;
    h.session->setCaptureBounds(1'000'000, 60.0);

    expect(h.session->goOnline() == scope::ScopeSession::Online::kWithoutCapture,
           "online with a recorder that subscribed to nothing reports no capture");
    expect(h.session->isOnline() && h.live_built == 1, "and is on the live source");
    expect(h.recorder_bytes == 1'000'000 && h.recorder_seconds == 60.0,
           "the recorder was built with the bounds set before it existed");
    expect(!h.session->hasCapture() && !h.session->captureUnsaved(),
           "an empty capture is not unsaved");
    expect(h.session->goOnline() == scope::ScopeSession::Online::kAlready, "online twice is a no-op");

    scope::ScopeRecorder* first = h.session->recorder();
    first->buffer().push(message(0));
    first->buffer().push(message(2'000'000'000));
    expect(h.session->hasCapture() && h.session->captureUnsaved(), "a pushed message is unsaved");

    h.log.clear();
    expect(h.session->goOffline() == scope::ScopeSession::Offline::kReviewingCapture,
           "going offline lands on the capture");
    expect(!h.session->isOnline() && h.session->source().caps().seekable,
           "the review is an offline, seekable source");
    expect(h.session->label().starts_with("session capture"), "and is named as the capture");
    expect(!h.log.empty() && h.log.back() == "changed", "and the window was told");

    // Back online from the review. The review reads the first recorder's
    // buffer, so the new source must be in place -- and the review destroyed --
    // while that recorder is still the session's.
    bool recorder_still_first = false;
    h.on_rebind = [&](scope::DataSource&) { recorder_still_first = h.session->recorder() == first; };
    expect(h.session->goOnline() == scope::ScopeSession::Online::kWithoutCapture,
           "online again from a review");
    expect(recorder_still_first,
           "the source is replaced before the recorder whose buffer the review reads");
    expect(h.session->recorder() != first || h.session->recorder()->buffer().size() == 0,
           "and the capture starts fresh");
    h.on_rebind = nullptr;

    h.log.clear();
    expect(h.session->goOffline() == scope::ScopeSession::Offline::kEmpty,
           "offline with nothing captured lands on nothing");
    expect(!h.session->isOnline() && !h.session->source().caps().seekable,
           "an empty source, not a review");
    expect(h.session->label().empty(), "which names nothing");
    expect(h.session->goOffline() == scope::ScopeSession::Offline::kAlready,
           "offline twice is a no-op");
}

void testTheSaveMarkIsAWatermark()
{
    Harness h;
    h.session->goOnline();
    h.session->recorder()->buffer().push(message(0));
    h.session->recorder()->buffer().push(message(3'000'000'000));

    const std::filesystem::path saved = scratchDirectory("saved");
    expect(h.session->saveCaptureTo(saved.string()), "the capture saves");
    expect(!h.session->captureUnsaved(), "and is then not unsaved");

    h.session->recorder()->buffer().push(message(4'000'000'000));
    expect(h.session->captureUnsaved(),
           "a message after the save is unsaved again -- a latch would lose it");
    expect(!h.session->saveCaptureTo(""), "an empty path does not save");

    // The saved bag opens, with a trailing slash too, and opening it keeps the
    // capture's buffer.
    const std::size_t captured = h.session->recorder()->buffer().size();
    const auto opened = h.session->openRecording(saved.string() + "/");
    expect(opened.has_value(), "the saved capture opens as a recording");
    if (opened)
    {
        expect(opened->duration_seconds > 2.9 && opened->duration_seconds < 3.1,
               "spanning the saved messages");
    }
    expect(h.session->label().starts_with(saved.filename().string() + " · "),
           "named by its directory, trailing slash or not");
    expect(!h.session->isOnline(), "a recording is offline");
    expect(h.session->recorder() != nullptr && h.session->recorder()->buffer().size() == captured,
           "and the capture's buffer survived the open");

    std::filesystem::remove_all(saved);
}

}  // namespace

int main()
{
    spdlog::set_default_logger(std::make_shared<spdlog::logger>(
        "test", std::make_shared<spdlog::sinks::null_sink_mt>()));

    testStartsOfflineWithoutBuildingAnything();
    testRebindRunsWhileTheOldSourceIsAlive();
    testAFailedOpenChangesNothing();
    testOnlineReviewAndBackAgain();
    testTheSaveMarkIsAWatermark();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
