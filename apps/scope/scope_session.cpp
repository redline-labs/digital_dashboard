#include "scope/scope_session.h"

#include "scope/empty_source.h"
#include "scope/live_zenoh_source.h"
#include "scope/recorded_source.h"
#include "scope/scope_recorder.h"
#include "scope/workspace.h"

#include "pub_sub/node_identity.h"

#include <spdlog/spdlog.h>

#include <filesystem>
#include <format>
#include <utility>

namespace scope
{

ScopeSession::Factories ScopeSession::busFactories()
{
    Factories factories;
    factories.live = []() -> std::unique_ptr<DataSource>
    {
        // Announce this process on the bus the first time it joins it, and not
        // before: constructed earlier (it was in main()), it opened a zenoh
        // session in a window that was supposed to be offline. Static so the
        // token does not flap when the user toggles offline and back.
        static const pub_sub::NodeIdentity node_identity("scope");
        static_cast<void>(node_identity);
        return std::make_unique<LiveZenohSource>();
    };
    factories.recorder = [](std::size_t max_bytes, double max_seconds)
    { return std::make_unique<ScopeRecorder>(max_bytes, max_seconds); };
    return factories;
}

ScopeSession::ScopeSession(Hooks hooks, Factories factories) :
    hooks_(std::move(hooks)),
    factories_(std::move(factories)),
    source_(std::make_unique<EmptySource>())
{
    // Held rather than read from a recorder: the recorder they configure is
    // built on the way online, possibly several times.
    const scope_workspace_t defaults;
    capture_max_bytes_ = defaults.max_capture_bytes;
    capture_max_seconds_ = defaults.max_capture_seconds;
}

ScopeSession::~ScopeSession() = default;

void ScopeSession::setSource(std::unique_ptr<DataSource> next)
{
    if (!next || next.get() == source_.get())
    {
        return;
    }
    if (hooks_.rebind)
    {
        hooks_.rebind(*next);
    }
    // Only now, with nothing holding a handle on it, is the old source
    // destroyed.
    source_ = std::move(next);
    if (hooks_.changed)
    {
        hooks_.changed();
    }
}

bool ScopeSession::isOnline() const
{
    return source_->caps().live;
}

bool ScopeSession::hasCapture() const
{
    return recorder_ != nullptr && recorder_->buffer().size() > 0;
}

bool ScopeSession::captureUnsaved() const
{
    // Anything pushed (or evicted -- eviction only happens on a push) since the
    // watermark means the file on disk no longer holds this session.
    return hasCapture() && recorder_->buffer().revision() != capture_saved_revision_;
}

void ScopeSession::setCaptureBounds(std::uint64_t max_bytes, double max_seconds)
{
    capture_max_bytes_ = max_bytes;
    capture_max_seconds_ = max_seconds;
    if (recorder_)
    {
        recorder_->buffer().setBounds(static_cast<std::size_t>(max_bytes), max_seconds);
    }
}

void ScopeSession::stopCapture()
{
    if (recorder_ != nullptr)
    {
        recorder_->stop();
    }
}

std::optional<ScopeSession::Opened> ScopeSession::openRecording(const std::string& directory)
{
    auto provider = std::make_unique<BagFileProvider>(directory);
    // Before anything is swapped: a failed open that had already replaced the
    // source would land on a review of nothing, which looks exactly like a
    // recording that turned out to be empty.
    if (!provider->isValid())
    {
        SPDLOG_ERROR("'{}' is not a readable bag directory.", directory);
        return std::nullopt;
    }

    Opened opened;
    opened.problems = provider->problems();
    const auto [t_begin, t_end] = provider->spanNanos();
    opened.duration_seconds =
        t_end > t_begin ? static_cast<double>(t_end - t_begin) / 1e9 : 0.0;

    // A bag is an offline source, so the capture stops -- but its buffer is
    // kept: a bag opened by mistake must not destroy the session's capture.
    stopCapture();

    // "a/b/" has no filename; its parent does.
    std::filesystem::path path(directory);
    if (path.filename().empty())
    {
        path = path.parent_path();
    }
    // Before the swap, so `changed` already sees the new label.
    label_ = std::format("{} · {:.0f} s", path.filename().string(), opened.duration_seconds);

    setSource(std::make_unique<RecordedSource>(std::move(provider)));
    return opened;
}

bool ScopeSession::reviewCapture()
{
    if (!hasCapture())
    {
        return false;
    }
    // ScopeRecorder::stop() drops only the subscriber, so the buffer the
    // CaptureProvider points into lives on with the recorder.
    stopCapture();
    const CaptureBuffer& buffer = recorder_->buffer();
    label_ = std::format("session capture · {:.0f} s", buffer.retainedSpanSeconds());
    setSource(std::make_unique<RecordedSource>(std::make_unique<CaptureProvider>(buffer)));
    return true;
}

bool ScopeSession::saveCaptureTo(const std::string& directory)
{
    if (!recorder_ || directory.empty())
    {
        return false;
    }
    // Read BEFORE the save. A message arriving while saveTo() walks the buffer
    // may or may not land in the file; taken first, it keeps the capture
    // "unsaved", which errs towards prompting.
    const std::uint64_t revision_at_save = recorder_->buffer().revision();
    if (!recorder_->saveTo(directory))
    {
        SPDLOG_ERROR("Failed to save the capture to '{}'.", directory);
        return false;
    }
    capture_saved_revision_ = revision_at_save;
    SPDLOG_INFO("Saved {} captured message(s) to '{}'.", recorder_->buffer().size(), directory);
    return true;
}

ScopeSession::Online ScopeSession::goOnline()
{
    if (isOnline())
    {
        return Online::kAlready;
    }
    label_.clear();

    // The source FIRST, then the recorder. The old source may be a review of
    // the old recorder's buffer, and setSource() destroys it only after every
    // panel has rebound; replacing the recorder first would free that buffer
    // under a source still in use.
    setSource(factories_.live());

    // Everything on the bus, with no exclusions: the point of a capture is that
    // a signal nobody thought to plot can still be added afterwards.
    recorder_ = factories_.recorder(static_cast<std::size_t>(capture_max_bytes_),
                                    capture_max_seconds_);
    // A fresh buffer is empty at its starting revision; the first push moves it
    // past the watermark and the capture reads as unsaved.
    capture_saved_revision_ = recorder_->buffer().revision();
    return recorder_->isValid() ? Online::kCapturing : Online::kWithoutCapture;
}

ScopeSession::Offline ScopeSession::goOffline()
{
    if (!isOnline())
    {
        return Offline::kAlready;
    }
    // Land on what was just recorded, which is what leaving online is almost
    // always for.
    if (reviewCapture())
    {
        return Offline::kReviewingCapture;
    }
    stopCapture();
    label_.clear();
    setSource(std::make_unique<EmptySource>());
    return Offline::kEmpty;
}

}  // namespace scope
