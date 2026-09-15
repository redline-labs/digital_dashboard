#ifndef SCOPE_RECORDED_SOURCE_H_
#define SCOPE_RECORDED_SOURCE_H_

#include "scope/data_source.h"

#include "bag/reader.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace scope
{

// Where recorded messages come from, underneath a RecordedSource.
//
// The seam exists so that reviewing a bag on disk and reviewing scope's own
// in-memory capture are ONE DataSource implementation with two backends, rather
// than two sources that would drift apart. Everything that is actually hard --
// decoding a signal once instead of once per scrub tick, refilling buffers
// across a backwards seek without violating SampleHistory's ordering
// precondition, mapping the recording's epoch onto the panels' -- is the same
// problem either way, and is solved once above this interface.
class RecordedProvider
{
  public:
    virtual ~RecordedProvider() = default;

    // Visit every message with `t0_ns <= log_time <= t1_ns`, in log_time order.
    // The visitor returns true to continue and FALSE TO STOP THE WALK -- which
    // is what lets a teardown abort a whole-recording pass instead of waiting
    // for it, and it matters because ~RecordedSource joins the worker from the
    // GUI thread.
    //
    // Called once per bound signal, on a background thread, never per frame.
    // That is a requirement rather than an observation: BagReader::forEach
    // constructs and opens an mcap::McapReader per part per call, and on a part
    // with no summary it falls back to scanning the whole data section. Driven
    // from a slider it would re-open files thirty times a second and re-scan a
    // torn recording every one of them.
    virtual void forEach(std::uint64_t t0_ns, std::uint64_t t1_ns,
                         const std::function<bool(const bag::BagMessage&)>& visit) = 0;

    // What the recording contains, from its index -- not by reading messages.
    virtual std::vector<TopicInfo> topics() const = 0;

    // [first, last] log_time in the recording, in nanoseconds since the UNIX
    // epoch. Equal values mean an empty or single-message recording.
    virtual std::pair<std::uint64_t, std::uint64_t> spanNanos() const = 0;

    // Bumped when topics() or spanNanos() would answer differently. Constant
    // for a file, which cannot grow; a live capture moves it as the head
    // advances.
    virtual std::uint64_t revision() const { return 1; }

    // Message counts per uniform bucket over [t0_ns, t1_ns], for the overview
    // strip's background. Same "cheap or decline" contract as
    // DataSource::density(), and for the same reason as the forEach() warning
    // above: this is reached from a widget, and a widget must never drive a
    // scan of the data section.
    //
    // False leaves `out` empty and the strip draws a plain band, which is the
    // right answer for a provider that would have to read to know.
    virtual bool density(std::uint64_t /*t0_ns*/, std::uint64_t /*t1_ns*/,
                         std::size_t /*buckets*/, std::vector<std::uint32_t>& out)
    {
        out.clear();
        return false;
    }
};

// A recording on disk, through bag::BagReader.
class BagFileProvider : public RecordedProvider
{
  public:
    explicit BagFileProvider(const std::string& directory);
    ~BagFileProvider() override;

    BagFileProvider(const BagFileProvider&) = delete;
    BagFileProvider& operator=(const BagFileProvider&) = delete;

    // False when the directory has no readable metadata.yaml. A bag with a
    // DAMAGED part is still valid -- see problems().
    bool isValid() const;

    // Anything wrong with the recording that is not fatal: a torn part, a part
    // missing from disk, a non-zero drop count. Worth showing, because each of
    // them changes how the data should be read -- a gap in a trace means
    // something different when the recorder is known to have dropped messages.
    const std::vector<std::string>& problems() const;

    void forEach(std::uint64_t t0_ns, std::uint64_t t1_ns,
                 const std::function<bool(const bag::BagMessage&)>& visit) override;
    std::vector<TopicInfo> topics() const override;
    std::pair<std::uint64_t, std::uint64_t> spanNanos() const override;

    // From the PART INDEX in metadata.yaml, opening no file at all. Each part
    // carries a message_count and a [t_begin, t_end], so its count is spread
    // uniformly across the buckets its own span overlaps.
    //
    // APPROXIMATE, and it reports so: "how many" is indexed and "where within a
    // part" is not, so a single-part recording draws as one flat block. Parts
    // roll at a size or duration limit, so a recording big enough to want an
    // overview has many of them and the shape is real.
    //
    // The alternative -- counting through forEach -- is precisely what the
    // warning on that method forbids, and it would be driven from a widget.
    bool density(std::uint64_t t0_ns, std::uint64_t t1_ns, std::size_t buckets,
                 std::vector<std::uint32_t>& out) override;

  private:
    std::unique_ptr<bag::BagReader> reader_;
    std::vector<std::string> no_problems_;
};

// A DataSource over a recording: scrubbable, seekable, and indistinguishable
// from the live one to everything above it.
//
// DECODE ONCE PER SIGNAL, NOT ONCE PER SCRUB TICK. This is the load-bearing
// decision and the reason the class is shaped the way it is. bind() starts a
// single pass over the whole recording on a background thread, evaluating that
// signal's expression into a flat std::vector<Sample> held here. Seeking is
// then a slice out of that vector. The cost is modest -- four hours of a 25 Hz
// signal is 360k samples, under 6 MB -- and the alternative is re-reading and
// re-decoding the file on every frame of a drag.
//
// The pass is asynchronous, so bind() returns a usable handle before any data
// exists. A trace is simply empty until its decode finishes, which is the same
// state a live signal is in before its publisher says anything, and every panel
// already draws it correctly.
//
// TIME. now() is SECONDS SINCE THE RECORDING STARTED, matching the live
// source's "seconds since construction" shape, so TimeBase::viewEnd(),
// viewBegin() and the panels' relative axis labels work with no change at all.
// The wall clock a recording genuinely has -- and the live source does not --
// is available through wallClockNanosAt() for the cursor readout.
class RecordedSource : public DataSource
{
  public:
    explicit RecordedSource(std::unique_ptr<RecordedProvider> provider);
    ~RecordedSource() override;

    RecordedSource(const RecordedSource&) = delete;
    RecordedSource& operator=(const RecordedSource&) = delete;

    SourceCaps caps() const override;
    std::vector<TopicInfo> topics() const override;
    std::uint64_t topicsRevision() const override;
    SignalHandle bind(const SignalKey& key, std::shared_ptr<SignalBuffer> into) override;
    void release(SignalHandle handle) override;

    // Raw streams do NOT follow bind()'s decode-the-whole-recording strategy,
    // and cannot: half an hour of CarPlay video is about 900 MB of payload
    // against the 6 MB four hours of a 25 Hz signal costs.
    //
    // Instead, one pass builds a payload-free index -- time and the consumer's
    // classifier flags per message, about 1.3 MB for that same half hour -- and
    // a seek loads ONE seek-point-to-seek-point window of payloads around the
    // position. Both run on the same worker thread, so a scrub never reads a
    // file from the render tick. A scrub that stays inside the loaded window
    // reads nothing at all.
    RawHandle bindRaw(const std::string& zenoh_key, pub_sub::schema_type_t schema,
                      std::shared_ptr<RawBuffer> into, RawClassifier classify = {}) override;
    void releaseRaw(RawHandle handle) override;

    double now() const override;

    // Converts the source's clock to the provider's UNIX nanoseconds and hands
    // the question on. That conversion is the whole reason it is overridden
    // here: nothing above this speaks the recording's epoch.
    bool density(double t0, double t1, std::size_t buckets,
                 std::vector<std::uint32_t>& out) override;

    void seek(double t) override;
    void setPlaying(bool playing) override;
    void setRate(double rate) override;
    void tick() override;

    // Absolute time at a position on this source's clock, for a readout. Zero
    // when the recording carries no timestamps at all.
    std::uint64_t wallClockNanosAt(double t) const;

    // How many bound signals are still being decoded. Zero means every trace is
    // showing everything the recording has for it -- which is what a test has to
    // wait for, and what a progress indicator reports.
    std::size_t decodesPending() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace scope

#endif  // SCOPE_RECORDED_SOURCE_H_
