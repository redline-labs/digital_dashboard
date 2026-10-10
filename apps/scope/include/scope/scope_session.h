#ifndef SCOPE_SCOPE_SESSION_H_
#define SCOPE_SCOPE_SESSION_H_

#include "scope/data_source.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace scope
{

class ScopeRecorder;

// Where scope's samples come from, and the capture it keeps while online.
//
// The window used to hold the source, the recorder and their save watermark
// next to its docks and menus, so the rules about the order they change in
// could only be exercised by building a window. They are lifetime rules, and
// each is a use-after-free when broken:
//
//   - Panels release against the OLD source before it is destroyed (`rebind`
//     runs first, the old source dies after it).
//   - A review of the capture reads the recorder's buffer, so the source is
//     replaced BEFORE the recorder is, and the recorder is destroyed after the
//     source (it is declared first).
//   - A failed open changes nothing.
//
// No dialogs and no status messages: an operation reports what happened and
// the window says it. Asking "discard the unsaved capture?" before goOnline()
// is the window's job too.
class ScopeSession
{
  public:
    struct Hooks
    {
        // Move everything bound to the source -- panels, the signal browser,
        // the time base -- to `next`. Runs while the old source is still alive.
        std::function<void(DataSource& next)> rebind;
        // The source has been replaced; refresh whatever renders its caps.
        std::function<void()> changed;
    };

    // What going online builds. Injected so the transitions can be tested
    // without a bus; busFactories() is the real thing.
    struct Factories
    {
        std::function<std::unique_ptr<DataSource>()> live;
        std::function<std::unique_ptr<ScopeRecorder>(std::size_t max_bytes, double max_seconds)>
            recorder;
    };
    static Factories busFactories();

    // Starts OFFLINE, on an EmptySource: constructing a session opens no zenoh
    // session, so an offline window is not on the bus.
    explicit ScopeSession(Hooks hooks, Factories factories = busFactories());
    ~ScopeSession();

    ScopeSession(const ScopeSession&) = delete;
    ScopeSession& operator=(const ScopeSession&) = delete;

    DataSource& source() { return *source_; }
    const DataSource& source() const { return *source_; }

    // The one place a source is replaced; every transition below goes through
    // it. A null or identical source is ignored.
    void setSource(std::unique_ptr<DataSource> next);

    // What an offline source is: a bag's directory name and duration, or the
    // capture's span; empty when online or when nothing is loaded. Kept here
    // because a RecordedSource does not know where it came from.
    const std::string& label() const { return label_; }

    // Derived from the source, never stored: online exactly when it is
    // tailing the bus, so the two cannot disagree after a failed open.
    bool isOnline() const;

    bool hasCapture() const;
    // Messages newer than the last save, or never saved.
    bool captureUnsaved() const;
    // The buffer revision at the last save, for a caller comparing it against a
    // CaptureBuffer::Stats it already took under one lock.
    std::uint64_t captureSavedRevision() const { return capture_saved_revision_; }
    ScopeRecorder* recorder() { return recorder_.get(); }
    const ScopeRecorder* recorder() const { return recorder_.get(); }

    // Applied to the current capture and to every later one.
    void setCaptureBounds(std::uint64_t max_bytes, double max_seconds);
    std::uint64_t captureMaxBytes() const { return capture_max_bytes_; }
    double captureMaxSeconds() const { return capture_max_seconds_; }

    struct Opened
    {
        double duration_seconds = 0.0;
        // A torn part, a drop count: things that change how a gap reads.
        std::vector<std::string> problems;
    };
    // Reviews a bag directory, stopping the capture but keeping its buffer.
    // nullopt when it is not a readable bag, with nothing changed.
    std::optional<Opened> openRecording(const std::string& directory);

    // Reviews the session's own capture, stopping it. False when nothing has
    // been captured.
    bool reviewCapture();

    // Writes the capture as a bag directory and moves the save watermark.
    bool saveCaptureTo(const std::string& directory);

    enum class Online
    {
        kAlready,
        kCapturing,
        // On the bus, but the capture could not subscribe: nothing to review.
        kWithoutCapture,
    };
    // Tails the bus and starts a FRESH capture, discarding the previous one.
    Online goOnline();

    enum class Offline
    {
        kAlready,
        kReviewingCapture,
        // Nothing was captured, so there is nothing to land on.
        kEmpty,
    };
    // Leaves the bus, landing on the capture when there is one.
    Offline goOffline();

  private:
    void stopCapture();

    Hooks hooks_;
    Factories factories_;

    // DECLARED BEFORE source_, so it is destroyed after it: a review's
    // RecordedSource reads this recorder's buffer from a worker thread its
    // destructor joins.
    std::unique_ptr<ScopeRecorder> recorder_;
    std::unique_ptr<DataSource> source_;

    // The buffer revision at the last save: a watermark, not a latch. A latch
    // set at the first save let everything captured after it go unprompted.
    std::uint64_t capture_saved_revision_ = 0;
    std::uint64_t capture_max_bytes_;
    double capture_max_seconds_;
    std::string label_;
};

}  // namespace scope

#endif  // SCOPE_SCOPE_SESSION_H_
