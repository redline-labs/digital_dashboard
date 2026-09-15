#include "scope/recorded_source.h"

#include "pub_sub/expression_evaluator.h"

#include <reflection/reflection.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <thread>

namespace scope
{

namespace
{

constexpr double kNanosPerSecond = 1e9;

}  // namespace

// ------------------------------------------------------------- BagFileProvider

BagFileProvider::BagFileProvider(const std::string& directory) :
    reader_(std::make_unique<bag::BagReader>(directory))
{
}

BagFileProvider::~BagFileProvider() = default;

bool BagFileProvider::isValid() const
{
    return reader_->isValid();
}

const std::vector<std::string>& BagFileProvider::problems() const
{
    return reader_->isValid() ? reader_->problems() : no_problems_;
}

void BagFileProvider::forEach(std::uint64_t t0_ns, std::uint64_t t1_ns,
                              const std::function<bool(const bag::BagMessage&)>& visit)
{
    if (!reader_->isValid())
    {
        return;
    }

    // Straight through: BagReader::forEach already stops on false, which is
    // what lets a teardown abort a pass over a large bag instead of decoding
    // the rest of it into a source that is being destroyed.
    reader_->forEach(t0_ns, t1_ns, visit);
}

std::vector<TopicInfo> BagFileProvider::topics() const
{
    std::vector<TopicInfo> out;
    if (!reader_->isValid())
    {
        return out;
    }

    // Straight from the index -- bag_topic_t maps onto TopicInfo field for
    // field. Nothing is read, which is what makes opening a multi-gigabyte
    // recording instant.
    for (const bag::bag_topic_t& topic : reader_->metadata().topics)
    {
        TopicInfo info;
        info.key = topic.key;
        info.schema = topic.schema;

        // A topic that was advertised for the whole recording and never
        // published is exactly what `reachable = false` means to the browser:
        // listed, because someone may have bound it, and greyed, because
        // binding it will produce nothing. That fact is unrecoverable from the
        // messages alone and is the reason the recorder snapshots
        // advertisements at all.
        info.reachable = !topic.advertised_only;

        out.push_back(std::move(info));
    }
    return out;
}

std::pair<std::uint64_t, std::uint64_t> BagFileProvider::spanNanos() const
{
    if (!reader_->isValid())
    {
        return {0, 0};
    }
    return {reader_->metadata().t_begin_ns, reader_->metadata().t_end_ns};
}

bool BagFileProvider::density(std::uint64_t t0_ns, std::uint64_t t1_ns, std::size_t buckets,
                              std::vector<std::uint32_t>& out)
{
    out.assign(buckets, 0);
    if (!reader_->isValid() || buckets == 0 || t1_ns <= t0_ns)
    {
        out.clear();
        return false;
    }

    const double span = static_cast<double>(t1_ns - t0_ns);
    bool any = false;

    for (const bag::bag_part_t& part : reader_->metadata().parts)
    {
        if (part.message_count == 0 || part.t_end_ns < t0_ns || part.t_begin_ns > t1_ns)
        {
            continue;
        }

        // Clip to the requested window before spreading, or a part hanging off
        // an edge donates its whole count to the buckets that ARE in range and
        // draws a spike where the recording merely continues.
        const std::uint64_t lo = std::max(part.t_begin_ns, t0_ns);
        const std::uint64_t hi = std::min(part.t_end_ns, t1_ns);

        const double first = static_cast<double>(lo - t0_ns) / span * static_cast<double>(buckets);
        const double last = static_cast<double>(hi - t0_ns) / span * static_cast<double>(buckets);

        auto lo_bucket = static_cast<std::size_t>(first);
        auto hi_bucket = static_cast<std::size_t>(last);
        lo_bucket = std::min(lo_bucket, buckets - 1);
        hi_bucket = std::min(hi_bucket, buckets - 1);

        // A part whose whole span lands inside one bucket, including the
        // degenerate single-message part where t_begin == t_end.
        const std::size_t covered = hi_bucket - lo_bucket + 1;
        const auto share = static_cast<std::uint32_t>(part.message_count / covered);
        for (std::size_t i = lo_bucket; i <= hi_bucket; ++i)
        {
            out[i] += share;
        }
        any = true;
    }

    // No overlapping part is a real answer -- an empty stretch of the
    // recording -- and is different from "cannot answer".
    return any || reader_->metadata().parts.empty();
}

// ------------------------------------------------------------- RecordedSource

namespace
{

// One bound signal: the decoded samples, and everything needed to produce them.
struct RecordedBinding
{
    SignalHandle handle = kInvalidSignal;
    SignalKey key;
    std::shared_ptr<SignalBuffer> buffer;

    // The registry name this binding will accept, resolved once at bind time.
    //
    // NOT ExpressionEvaluator::checkPublishedSchema(). That takes a whole
    // encoding string ("application/capnp;EngineRpm") so it can tell "published
    // as something else" apart from "published with no schema named" -- and
    // BagMessage::schema is the bare registry name. Passed through, it would
    // match neither branch and check nothing at all, silently, which is the one
    // outcome worse than not checking: capnp decodes against whatever schema it
    // is handed and yields a plausible wrong number rather than an error.
    std::string expected_schema;

    // exprtk binds field slots into its symbol table BY ADDRESS, so this is
    // non-movable and has to be held indirectly.
    std::unique_ptr<pub_sub::ExpressionEvaluator> evaluator;

    // The whole recording, decoded once. Written by the worker thread before
    // `ready` is set, read by the GUI thread after -- so the flag is the
    // hand-off and nothing needs a lock around the vector itself.
    std::vector<Sample> samples;
    bool ready = false;

    // Where the last refill left the buffer, so forward playback appends only
    // the newly-reached tail instead of rebuilding the window every frame.
    std::size_t filled_to = 0;
    bool filled = false;
};

// A whole topic bound as bytes, with the two-stage arrangement a scrub over
// video forces.
//
// THE NUMERIC PATH'S STRATEGY DOES NOT TRANSFER, and that is the whole reason
// this type exists rather than another RecordedBinding. RecordedBinding decodes
// the entire recording into a flat vector at bind time, which is right when a
// sample is sixteen bytes: four hours of a 25 Hz signal is under 6 MB. The same
// move on a video topic is not a bigger version of the same thing -- half an
// hour of CarPlay at 4 Mbit is about 900 MB, and it would be read and held to
// show one frame.
//
// So: an index of everything, holding NO payloads, and one GOP of payloads at a
// time.
struct RecordedRawBinding
{
    SignalHandle handle = kInvalidSignal;
    std::string zenoh_key;
    std::string expected_schema;
    RawClassifier classify;
    std::shared_ptr<RawBuffer> buffer;

    // One entry per message on this key. ~24 bytes each, so half an hour of
    // 30 fps video is about 1.3 MB -- against the ~900 MB the payloads would be.
    // This is what the scrubber draws its keyframe ticks from and what a seek
    // binary-searches to find the GOP it must start decoding at.
    struct IndexEntry
    {
        double t = 0.0;
        std::uint32_t flags = 0;
        std::uint32_t bytes = 0;
    };
    std::vector<IndexEntry> index;
    bool ready = false;

    // ------------------------------------------------ the window, and its request
    //
    // Loading runs on the WORKER, never on the GUI thread, because it is file
    // I/O: RecordedProvider::forEach opens an mcap reader per part per call.
    // Doing it inline would put exactly the widget-driven file read that
    // method's warning forbids on the render tick.

    // Index of the seek point the loaded (or wanted) window starts at. The
    // identity of a window -- a scrub that stays inside one GOP leaves this
    // alone and reads nothing at all.
    std::size_t want_start = 0;
    bool want_valid = false;
    bool request_queued = false;

    std::size_t loaded_start = 0;
    bool loaded = false;

    // Filled by the worker, moved into the buffer by the GUI thread on the next
    // tick. Staged rather than pushed directly because RawBuffer's history half
    // is GUI-owned, exactly as SignalBuffer's is.
    std::vector<RawMessage> staged;
    bool staged_ready = false;

    // Bytes the last window load read, for the panel's stats. A window that is
    // quietly enormous is worth being able to see.
    std::uint64_t last_window_bytes = 0;
};

}  // namespace

struct RecordedSource::Impl
{
    std::unique_ptr<RecordedProvider> provider;

    std::uint64_t t_begin_ns = 0;
    std::uint64_t t_end_ns = 0;

    // Seconds since the recording started. The panels' whole world.
    double position = 0.0;

    bool playing = false;
    double rate = 1.0;
    std::chrono::steady_clock::time_point last_tick = std::chrono::steady_clock::now();

    SignalHandle next_handle = 1;  // 0 is kInvalidSignal.

    // Guards `bindings` and the queue. Held only around map lookups and the
    // ready/samples hand-off -- never across a decode pass, which is minutes of
    // file I/O on a large recording.
    mutable std::mutex mutex;
    std::map<SignalHandle, std::shared_ptr<RecordedBinding>> bindings;

    std::map<SignalHandle, std::shared_ptr<RecordedRawBinding>> raw_bindings;

    // ONE worker thread, not one per bind. RecordedProvider is not thread-safe
    // -- BagFileProvider holds an mcap reader with its own decompression buffers
    // -- so decodes are serialized by construction rather than by a lock nobody
    // would remember to take. The raw work joins the same thread for the same
    // reason: a second one would touch the same reader.
    struct Job
    {
        enum class Kind
        {
            // Decode one signal's expression over the whole recording.
            DecodeSignal,

            // Build one raw stream's payload-free index over the whole recording.
            IndexRaw,

            // Load one GOP of payloads for one raw stream. Cheap and frequent,
            // unlike the two above.
            LoadWindow,
        };

        Kind kind = Kind::DecodeSignal;
        std::shared_ptr<RecordedBinding> signal;
        std::shared_ptr<RecordedRawBinding> raw;
    };

    std::deque<Job> queue;
    std::condition_variable work;

    // Atomic because the whole-recording passes poll it per message WITHOUT the
    // lock. The destructor joins the worker; before this was checked inside
    // decode()/indexRaw(), destroying a source mid-pass blocked the GUI thread
    // (setSource destroys the old source there) until a full scan of the
    // recording finished. Once set, the visit callback returns false and the
    // provider stops the walk.
    std::atomic<bool> stopping{false};

    // Counts the two WHOLE-RECORDING passes only, which is what decodesPending()
    // means and what a test waits on. A window load is a bounded read of one
    // GOP and finishes in milliseconds; counting it would make the flag flicker
    // during every scrub and turn "wait until zero" into a race.
    std::size_t pending = 0;

    std::thread worker;

    double duration() const
    {
        return t_end_ns > t_begin_ns
                   ? static_cast<double>(t_end_ns - t_begin_ns) / kNanosPerSecond
                   : 0.0;
    }

    // ONE pass over the recording serving EVERY queued whole-recording job:
    // all pending signal decodes and raw indexes together. Runs on the worker
    // thread.
    //
    // This used to be one full pass PER binding, serialized -- so opening an
    // eight-trace workspace over a bag read (and, for a torn part, re-scanned)
    // the whole file eight times. The per-message cost of dispatching to
    // several bindings is a map lookup; the per-pass cost of a multi-gigabyte
    // mcap is I/O and decompression, and that is what a batch amortizes.
    void decodeBatch(const std::vector<Job>& batch)
    {
        struct SignalWork
        {
            std::shared_ptr<RecordedBinding> binding;
            std::vector<Sample> decoded;
        };
        struct RawWork
        {
            std::shared_ptr<RecordedRawBinding> binding;
            std::vector<RecordedRawBinding::IndexEntry> built;
        };

        std::vector<SignalWork> signal_work;
        std::vector<RawWork> raw_work;
        // Key -> the work items bound to it, so a message costs one ordered
        // lookup rather than a string compare against every binding.
        // std::map with std::less<>, because BagMessage::key is a view and a
        // transparent comparator avoids a per-message std::string.
        std::map<std::string, std::vector<std::size_t>, std::less<>> signals_by_key;
        std::map<std::string, std::vector<std::size_t>, std::less<>> raw_by_key;

        for (const Job& job : batch)
        {
            if (job.kind == Job::Kind::DecodeSignal)
            {
                signals_by_key[job.signal->key.zenoh_key].push_back(signal_work.size());
                signal_work.push_back(SignalWork{job.signal, {}});
            }
            else
            {
                raw_by_key[job.raw->zenoh_key].push_back(raw_work.size());
                raw_work.push_back(RawWork{job.raw, {}});
            }
        }

        provider->forEach(
            t_begin_ns, t_end_ns,
            [&](const bag::BagMessage& message) -> bool
            {
                if (stopping.load(std::memory_order_relaxed))
                {
                    return false;
                }

                // log_time, not publish_time. log_time is the recorder's own
                // clock and is monotone; publish_time is the publisher's wall
                // clock and can step backwards when NTP disciplines it, or be
                // plainly wrong on a unit that booted with a dead RTC. An axis
                // that jumps backwards mid-trace is worse than one measured
                // from a slightly delayed origin -- and it would break
                // SampleHistory's ordering precondition outright.
                const double t =
                    static_cast<double>(message.log_time_ns - t_begin_ns) / kNanosPerSecond;

                if (const auto found = signals_by_key.find(message.key);
                    found != signals_by_key.end())
                {
                    for (const std::size_t i : found->second)
                    {
                        RecordedBinding& binding = *signal_work[i].binding;

                        // Recorded with a different schema than the binding
                        // expects. Skipped rather than decoded: capnp will
                        // happily read these bytes against the wrong schema
                        // and produce a number.
                        if (!message.schema.empty() &&
                            message.schema != binding.expected_schema)
                        {
                            continue;
                        }

                        // A span, so nothing is copied. nullopt drops the
                        // sample rather than pushing zero: a gap in the line is
                        // honest, a spike that never happened is not.
                        if (const std::optional<double> value =
                                binding.evaluator->evaluateToDouble(message.payload))
                        {
                            signal_work[i].decoded.push_back(Sample{t, *value});
                        }
                    }
                }

                if (const auto found = raw_by_key.find(message.key); found != raw_by_key.end())
                {
                    for (const std::size_t i : found->second)
                    {
                        RecordedRawBinding& binding = *raw_work[i].binding;

                        // Same rule as the numeric path: a message recorded
                        // under a different schema is skipped rather than
                        // handed over. A decoder fed the wrong stream produces
                        // a plausible mess, not an error.
                        if (!message.schema.empty() &&
                            message.schema != binding.expected_schema)
                        {
                            continue;
                        }

                        RecordedRawBinding::IndexEntry entry;
                        entry.t = t;
                        entry.bytes = static_cast<std::uint32_t>(message.payload.size());
                        if (binding.classify)
                        {
                            entry.flags = binding.classify(message.payload);
                        }
                        raw_work[i].built.push_back(entry);
                    }
                }

                return true;
            });

        const std::lock_guard<std::mutex> guard(mutex);
        for (SignalWork& item : signal_work)
        {
            item.binding->samples = std::move(item.decoded);
            item.binding->ready = true;

            // Cleared so the next refill rebuilds the window from scratch. The
            // buffer is empty at this point and `filled_to` describes a vector
            // that did not exist when it was last set.
            item.binding->filled = false;
            item.binding->filled_to = 0;
        }
        for (RawWork& item : raw_work)
        {
            item.binding->index = std::move(item.built);
            item.binding->ready = true;

            // The window that was loaded, if any, described an index that did
            // not exist when it was recorded.
            item.binding->loaded = false;
            item.binding->staged_ready = false;
        }
    }

    // Load the payloads for ONE GOP: from the seek point at `want_start` up to
    // (but not including) the next one. Runs on the worker thread.
    //
    // A whole GOP rather than "up to the playhead", so that scrubbing within it
    // and playing forward through it read the file exactly once. At a two-second
    // GOP that is about sixty access units.
    void loadWindow(const std::shared_ptr<RecordedRawBinding>& binding)
    {
        std::size_t start = 0;
        std::uint64_t from_ns = 0;
        std::uint64_t to_ns = 0;
        std::string key;
        std::string schema;
        RawClassifier classify;

        {
            const std::lock_guard<std::mutex> guard(mutex);
            binding->request_queued = false;
            if (!binding->ready || !binding->want_valid || binding->index.empty())
            {
                return;
            }

            // The LATEST request, not the one that was queued. A drag makes many
            // and only the last matters; servicing the stale one would load a
            // window the user has already scrubbed away from.
            start = binding->want_start;
            if (start >= binding->index.size())
            {
                return;
            }

            // Up to the NEXT GOP's start, exclusive. One nanosecond short of
            // it, because forEach's range is closed at both ends.
            //
            // `start` is the beginning of the current window, which may be a
            // run of preamble messages sitting in front of this GOP's own seek
            // point. So the scan has to step over that seek point first --
            // stopping at it would make the window everything before this GOP's
            // keyframe, which is a window with nothing decodable in it.
            std::size_t own = start;
            while (own < binding->index.size() &&
                   (binding->index[own].flags & RawMessage::kSeekPoint) == 0)
            {
                ++own;
            }

            std::size_t next = (own < binding->index.size()) ? own + 1 : binding->index.size();
            while (next < binding->index.size() &&
                   (binding->index[next].flags & RawMessage::kSeekPoint) == 0)
            {
                ++next;
            }

            // And stop short of the NEXT GOP's preamble rather than swallowing
            // it, so the following window still begins with its own parameter
            // sets. gopStartFor() walks back over exactly the same run.
            while (next > own + 1 &&
                   (binding->index[next - 1].flags & RawMessage::kPreamble) != 0)
            {
                --next;
            }

            from_ns = t_begin_ns +
                      static_cast<std::uint64_t>(binding->index[start].t * kNanosPerSecond);
            to_ns = next < binding->index.size()
                        ? t_begin_ns + static_cast<std::uint64_t>(binding->index[next].t *
                                                                  kNanosPerSecond) -
                              1
                        : t_end_ns;

            key = binding->zenoh_key;
            schema = binding->expected_schema;
            classify = binding->classify;
        }

        std::vector<RawMessage> loaded;
        std::uint64_t bytes = 0;

        provider->forEach(from_ns, to_ns,
                          [&](const bag::BagMessage& message) -> bool
                          {
                              if (stopping.load(std::memory_order_relaxed))
                              {
                                  return false;
                              }
                              if (message.key != key)
                              {
                                  return true;
                              }
                              if (!message.schema.empty() && message.schema != schema)
                              {
                                  return true;
                              }

                              RawMessage out;
                              out.t = static_cast<double>(message.log_time_ns - t_begin_ns) /
                                      kNanosPerSecond;
                              out.payload.assign(message.payload.begin(), message.payload.end());
                              if (classify)
                              {
                                  out.flags = classify(out.payload);
                              }
                              bytes += out.payload.size();
                              loaded.push_back(std::move(out));
                              return true;
                          });

        const std::lock_guard<std::mutex> guard(mutex);

        // Dropped if the request moved on while this was reading. Publishing it
        // anyway would put the buffer somewhere the caller has already left, and
        // the newer request is already queued behind this one.
        if (binding->want_valid && binding->want_start == start)
        {
            binding->staged = std::move(loaded);
            binding->staged_ready = true;
            binding->loaded_start = start;
            binding->last_window_bytes = bytes;
        }
    }

    // Drop every queued job matching `match`, returning how many were counted
    // in `pending`. Called with the mutex held.
    template <typename Match>
    std::size_t eraseQueued(Match match)
    {
        std::size_t counted = 0;
        for (const Job& job : queue)
        {
            if (match(job) && job.kind != Job::Kind::LoadWindow)
            {
                ++counted;
            }
        }
        queue.erase(std::remove_if(queue.begin(), queue.end(), match), queue.end());
        return counted;
    }

    // Drop every queued job for one raw binding, reporting the whole-recording
    // passes and the window loads separately because only the first kind is
    // counted in `pending`. Called with the mutex held.
    std::pair<std::size_t, std::size_t> eraseQueuedRaw(
        const std::shared_ptr<RecordedRawBinding>& binding)
    {
        std::size_t passes = 0;
        std::size_t windows = 0;
        for (const Job& job : queue)
        {
            if (job.raw != binding)
            {
                continue;
            }
            if (job.kind == Job::Kind::LoadWindow)
            {
                ++windows;
            }
            else
            {
                ++passes;
            }
        }
        queue.erase(std::remove_if(queue.begin(), queue.end(),
                                   [&binding](const Job& job) { return job.raw == binding; }),
                    queue.end());
        return {passes, windows};
    }

    void run()
    {
        for (;;)
        {
            Job job;
            std::vector<Job> batch;
            {
                std::unique_lock<std::mutex> lock(mutex);
                work.wait(lock, [this]() { return stopping || !queue.empty(); });
                if (stopping)
                {
                    return;
                }
                job = queue.front();
                queue.pop_front();

                // A whole-recording job takes EVERY other queued whole-
                // recording job with it, so N bindings queued together cost
                // one pass instead of N. Window loads stay solo -- they are
                // bounded reads of one GOP and must not wait behind a batch
                // that grew while one was queued.
                if (job.kind != Job::Kind::LoadWindow)
                {
                    batch.push_back(job);
                    for (auto it = queue.begin(); it != queue.end();)
                    {
                        if (it->kind != Job::Kind::LoadWindow)
                        {
                            batch.push_back(*it);
                            it = queue.erase(it);
                        }
                        else
                        {
                            ++it;
                        }
                    }
                }
            }

            if (batch.empty())
            {
                loadWindow(job.raw);
                continue;
            }

            decodeBatch(batch);

            // Only the whole-recording passes are counted -- see `pending`.
            // The whole batch at once: every binding in it just became ready.
            {
                const std::lock_guard<std::mutex> guard(mutex);
                pending -= batch.size();
            }
        }
    }

    // Load `binding`'s buffer with the window ending at `position`.
    //
    // `force` rebuilds it wholesale; otherwise a forward move appends only what
    // is newly in range. The distinction is not an optimisation detail: a
    // BACKWARDS move must rebuild, because appending older samples after newer
    // ones is exactly the non-decreasing-time violation SampleHistory's
    // lowerBound() cannot detect and would answer wrongly for ever after.
    // Only ever called with a binding whose `ready` was observed UNDER the
    // mutex. That is what makes reading `samples` here without a lock safe: the
    // worker fills the vector and sets the flag while holding it, so acquiring
    // it afterwards is the happens-before edge, and a binding is decoded exactly
    // once so the vector never changes again.
    void refill(const std::shared_ptr<RecordedBinding>& binding, bool force)
    {
        const std::vector<Sample>& samples = binding->samples;
        const double history = binding->buffer->historySeconds();
        const double from = history > 0.0 ? position - history
                                          : -std::numeric_limits<double>::infinity();

        const auto at_or_after = [&samples](double t) {
            return static_cast<std::size_t>(
                std::lower_bound(samples.begin(), samples.end(), t,
                                 [](const Sample& sample, double bound)
                                 { return sample.t < bound; }) -
                samples.begin());
        };

        // One past the last sample at or before `position`. upper_bound rather
        // than lower_bound so a sample stamped exactly at the playback head is
        // included -- the same closed-at-the-top rule BagReader's ranges use.
        const std::size_t end = static_cast<std::size_t>(
            std::upper_bound(samples.begin(), samples.end(), position,
                             [](double bound, const Sample& sample)
                             { return bound < sample.t; }) -
            samples.begin());

        if (force || !binding->filled || end < binding->filled_to)
        {
            const std::size_t begin = at_or_after(from);
            binding->buffer->replaceHistory(
                std::span<const Sample>(samples.data() + begin, end - begin));
        }
        else if (end > binding->filled_to)
        {
            binding->buffer->append(std::span<const Sample>(
                samples.data() + binding->filled_to, end - binding->filled_to));
        }

        binding->filled_to = end;
        binding->filled = true;
    }

    void refillAll(bool force)
    {
        // `ready` is read HERE, under the mutex, and the ones that are not are
        // simply left out. Checking it inside refill() would be a read racing
        // the worker's write -- and the failure would not be a crash but a
        // half-decoded vector drawn as if it were the whole signal.
        std::vector<std::shared_ptr<RecordedBinding>> decoded;
        {
            const std::lock_guard<std::mutex> guard(mutex);
            decoded.reserve(bindings.size());
            for (const auto& [handle, binding] : bindings)
            {
                if (binding->ready && binding->buffer)
                {
                    decoded.push_back(binding);
                }
            }
        }

        for (const std::shared_ptr<RecordedBinding>& binding : decoded)
        {
            refill(binding, force);
        }
    }

    // The GOP that contains `position`, as an index into `entries`: the last
    // seek point at or before it, extended backwards over the preamble messages
    // immediately in front of it.
    //
    // The backwards extension is what carries H.264 parameter sets into the
    // window. They are published just ahead of the keyframe they describe, and a
    // window starting exactly at the keyframe would leave them one message
    // behind -- which decodes to nothing at all until the NEXT keyframe.
    // Only immediately-adjacent ones, so a producer that sends its preamble once
    // an hour cannot turn a GOP-sized read into a whole-recording one.
    static bool gopStartFor(const std::vector<RecordedRawBinding::IndexEntry>& entries,
                            double position, std::size_t& start)
    {
        if (entries.empty())
        {
            return false;
        }

        // Last entry at or before `position`.
        std::size_t at = entries.size();
        {
            std::size_t low = 0;
            std::size_t high = entries.size();
            while (low < high)
            {
                const std::size_t mid = low + (high - low) / 2;
                if (entries[mid].t <= position)
                {
                    low = mid + 1;
                }
                else
                {
                    high = mid;
                }
            }
            if (low == 0)
            {
                return false;
            }
            at = low - 1;
        }

        // Walk back to the seek point this message depends on.
        std::size_t seek_point = at;
        while ((entries[seek_point].flags & RawMessage::kSeekPoint) == 0)
        {
            if (seek_point == 0)
            {
                // Nothing decodable at or before here. A stream whose first
                // seek point is later than this reads as empty, which is the
                // honest answer -- it is the same state a live subscriber is in
                // before its first keyframe arrives.
                return false;
            }
            --seek_point;
        }

        while (seek_point > 0 && (entries[seek_point - 1].flags & RawMessage::kPreamble) != 0)
        {
            --seek_point;
        }

        start = seek_point;
        return true;
    }

    // Publish anything the worker finished, then ask it for a different window
    // if the position has moved out of the loaded one.
    //
    // `force` re-publishes the loaded window even when the GOP has not changed,
    // which is what a BACKWARDS scrub inside one GOP needs: the buffer is
    // correct but the consumer has to re-run its decode from the start of it.
    void refillAllRaw(bool force)
    {
        std::vector<std::shared_ptr<RecordedRawBinding>> to_publish;
        std::vector<std::shared_ptr<RecordedRawBinding>> to_request;

        {
            const std::lock_guard<std::mutex> guard(mutex);

            for (const auto& [handle, binding] : raw_bindings)
            {
                if (!binding->buffer)
                {
                    continue;
                }

                if (binding->staged_ready)
                {
                    to_publish.push_back(binding);
                    continue;
                }

                if (!binding->ready)
                {
                    continue;
                }

                std::size_t start = 0;
                if (!gopStartFor(binding->index, position, start))
                {
                    binding->want_valid = false;
                    continue;
                }

                const bool changed = !binding->loaded || binding->loaded_start != start;
                if (!changed && !force)
                {
                    continue;
                }

                binding->want_start = start;
                binding->want_valid = true;

                // At most one queued request per binding. A drag makes one per
                // render tick and the worker reads the LATEST want_start when it
                // gets there, so queueing another would only make it do the same
                // read twice.
                if (!binding->request_queued && changed)
                {
                    binding->request_queued = true;
                    to_request.push_back(binding);
                }
            }

            for (const std::shared_ptr<RecordedRawBinding>& binding : to_request)
            {
                queue.push_back(Job{Job::Kind::LoadWindow, nullptr, binding});
            }
        }

        if (!to_request.empty())
        {
            work.notify_one();
        }

        for (const std::shared_ptr<RecordedRawBinding>& binding : to_publish)
        {
            std::vector<RawMessage> staged;
            {
                const std::lock_guard<std::mutex> guard(mutex);
                staged = std::move(binding->staged);
                binding->staged = {};
                binding->staged_ready = false;
                binding->loaded = true;
            }

            // Wholesale, always. A window is one GOP and arrives whole, so there
            // is no forward-append case to optimise -- and replaceHistory is
            // what keeps RawHistory::lowerBound()'s non-decreasing precondition
            // true across a backwards scrub.
            binding->buffer->replaceHistory(std::move(staged));
        }
    }
};

RecordedSource::RecordedSource(std::unique_ptr<RecordedProvider> provider) :
    impl_(std::make_unique<Impl>())
{
    impl_->provider = std::move(provider);

    const auto [begin, end] = impl_->provider->spanNanos();
    impl_->t_begin_ns = begin;
    impl_->t_end_ns = end;

    // AT THE END, not the beginning. A recording is opened to look at what
    // happened, and what happened is usually at the end -- the same reason a log
    // viewer opens on the tail. Starting at zero shows an empty window for a
    // recording whose retention is shorter than its duration, which reads as
    // "this bag has no data in it".
    impl_->position = impl_->duration();

    impl_->worker = std::thread([this]() { impl_->run(); });
}

RecordedSource::~RecordedSource()
{
    {
        const std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->stopping = true;
        impl_->queue.clear();
    }
    impl_->work.notify_all();

    // Joined before anything else is destroyed. The worker holds shared_ptrs to
    // bindings, which hold shared_ptrs to SignalBuffers a panel may also be
    // draining -- and it touches the provider on every message. Letting the
    // members go first is a use-after-free on every teardown, which is the same
    // ordering rule KeySubscription documents on the live side.
    if (impl_->worker.joinable())
    {
        impl_->worker.join();
    }
}

SourceCaps RecordedSource::caps() const
{
    SourceCaps caps;
    caps.live = false;
    caps.seekable = true;
    caps.t_begin = 0.0;
    caps.t_end = impl_->duration();
    return caps;
}

std::vector<TopicInfo> RecordedSource::topics() const
{
    return impl_->provider->topics();
}

std::uint64_t RecordedSource::topicsRevision() const
{
    return impl_->provider->revision();
}

SignalHandle RecordedSource::bind(const SignalKey& key, std::shared_ptr<SignalBuffer> into)
{
    if (key.zenoh_key.empty() || key.value_expression.empty() || !into)
    {
        SPDLOG_ERROR("Refusing to bind a signal with an empty key, expression or buffer.");
        return kInvalidSignal;
    }

    auto evaluator = std::make_unique<pub_sub::ExpressionEvaluator>(
        key.schema_type, key.value_expression, key.zenoh_key);

    // Checked here rather than on the worker, so a bad expression is a definite
    // no immediately instead of a handle whose decode quietly produces nothing.
    if (!evaluator->isValid())
    {
        return kInvalidSignal;
    }

    auto binding = std::make_shared<RecordedBinding>();
    binding->key = key;
    binding->buffer = std::move(into);
    binding->evaluator = std::move(evaluator);
    binding->expected_schema =
        std::string(reflection::enum_traits<pub_sub::schema_type_t>::to_string(key.schema_type));

    {
        const std::lock_guard<std::mutex> guard(impl_->mutex);
        binding->handle = impl_->next_handle++;
        impl_->bindings.emplace(binding->handle, binding);
        impl_->queue.push_back(Impl::Job{Impl::Job::Kind::DecodeSignal, binding, nullptr});
        ++impl_->pending;
    }
    impl_->work.notify_one();

    SPDLOG_DEBUG("Bound recorded signal {} to '{}' ({}), expression '{}'; decoding.",
                 binding->handle, key.zenoh_key, reflection::enum_to_string(key.schema_type),
                 key.value_expression);
    return binding->handle;
}

RawHandle RecordedSource::bindRaw(const std::string& zenoh_key, pub_sub::schema_type_t schema,
                                  std::shared_ptr<RawBuffer> into, RawClassifier classify)
{
    if (zenoh_key.empty() || !into)
    {
        SPDLOG_ERROR("Refusing to bind a raw stream with an empty key or buffer.");
        return kInvalidRaw;
    }

    auto binding = std::make_shared<RecordedRawBinding>();
    binding->zenoh_key = zenoh_key;
    binding->expected_schema =
        std::string(reflection::enum_traits<pub_sub::schema_type_t>::to_string(schema));
    binding->classify = std::move(classify);
    binding->buffer = std::move(into);

    {
        const std::lock_guard<std::mutex> guard(impl_->mutex);
        binding->handle = impl_->next_handle++;
        impl_->raw_bindings.emplace(binding->handle, binding);
        impl_->queue.push_back(Impl::Job{Impl::Job::Kind::IndexRaw, nullptr, binding});
        ++impl_->pending;
    }
    impl_->work.notify_one();

    SPDLOG_DEBUG("Bound recorded raw stream {} to '{}' ({}); indexing.", binding->handle,
                 zenoh_key, binding->expected_schema);
    return RawHandle{binding->handle};
}

void RecordedSource::releaseRaw(RawHandle handle)
{
    const std::lock_guard<std::mutex> guard(impl_->mutex);

    const auto found = impl_->raw_bindings.find(handle.value);
    if (found == impl_->raw_bindings.end())
    {
        return;
    }

    // Same reasoning as release(): a pass already running finishes into a
    // binding nothing reads, which is harmless. Only the not-yet-started jobs
    // are worth dropping.
    //
    // A LoadWindow job does not count toward `pending`, so only the index pass
    // may decrement it -- which is why eraseQueued reports the two separately.
    const auto [erased_passes, erased_windows] = impl_->eraseQueuedRaw(found->second);
    impl_->pending -= erased_passes;
    static_cast<void>(erased_windows);

    impl_->raw_bindings.erase(found);
}

void RecordedSource::release(SignalHandle handle)
{
    const std::lock_guard<std::mutex> guard(impl_->mutex);

    const auto found = impl_->bindings.find(handle);
    if (found == impl_->bindings.end())
    {
        return;
    }

    // Dropped from the queue if it has not started. A decode already running
    // finishes into a binding nothing reads any more, which costs a pass over
    // the file and is harmless -- the alternative is a cancellation flag checked
    // per message for a case that only happens when a signal is removed within
    // seconds of being added.
    //
    // AND `pending` COMES DOWN WITH THEM. This used to erase the jobs and leave
    // the count, so releasing a signal before its decode started left
    // decodesPending() permanently above zero -- and that number is exactly what
    // a test, and `scope.source`, wait on to know the picture is finished.
    // Rebinding every panel across a source swap is the case that hits it.
    impl_->pending -= impl_->eraseQueued(
        [&found](const Impl::Job& job) { return job.signal == found->second; });

    impl_->bindings.erase(found);
}

double RecordedSource::now() const
{
    return impl_->position;
}

bool RecordedSource::density(double t0, double t1, std::size_t buckets,
                             std::vector<std::uint32_t>& out)
{
    out.clear();
    if (impl_->provider == nullptr || t1 <= t0)
    {
        return false;
    }

    // Seconds-since-the-recording-started, out to the provider's UNIX
    // nanoseconds and no further. This conversion is the only reason the method
    // is here rather than being called on the provider directly: everything
    // above RecordedSource speaks the source's clock and must not learn about
    // the recording's epoch to draw a histogram.
    const auto to_nanos = [this](double t) {
        return impl_->t_begin_ns +
               static_cast<std::uint64_t>(std::max(t, 0.0) * kNanosPerSecond);
    };

    return impl_->provider->density(to_nanos(t0), to_nanos(t1), buckets, out);
}

void RecordedSource::seek(double t)
{
    impl_->position = std::clamp(t, 0.0, impl_->duration());

    // Force, because a seek may go backwards and an incremental refill after one
    // would append older samples on top of newer ones -- which SampleHistory's
    // binary search cannot detect and would answer wrongly from then on.
    impl_->refillAll(/*force=*/true);

    // Forced too, and for the analogous reason. A seek inside the loaded GOP
    // reads no file -- the window is already right -- but the consumer must be
    // told to run its decode again from the start of it, because a decoder
    // cannot go backwards through the frames it has already consumed.
    impl_->refillAllRaw(/*force=*/true);
}

void RecordedSource::setPlaying(bool playing)
{
    if (playing == impl_->playing)
    {
        return;
    }
    impl_->playing = playing;

    // Restarted, not accumulated: the interval since the last tick was spent
    // stopped, and folding it in would make pressing play jump the head forward
    // by however long the recording sat paused.
    impl_->last_tick = std::chrono::steady_clock::now();
}

void RecordedSource::setRate(double rate)
{
    impl_->rate = rate;
}

void RecordedSource::tick()
{
    const auto tick_at = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(tick_at - impl_->last_tick).count();
    impl_->last_tick = tick_at;

    if (impl_->playing)
    {
        // Clamped at the end, and that is all this does about it. Whether
        // reaching the end STOPS playback is TimeBase's call, not this one's --
        // it owns the playing flag the transport bar renders from, and a source
        // that quietly cleared its own would leave the Play button claiming to
        // still be running. TimeBase watches for now() == t_end instead.
        impl_->position = std::min(impl_->position + elapsed * impl_->rate, impl_->duration());
    }

    // Every tick, playing or not: a decode that finished since the last one has
    // a buffer to fill, and it has no other way to say so. The same is true of a
    // window load, which also completes on the worker with nothing to announce
    // it.
    impl_->refillAll(/*force=*/false);
    impl_->refillAllRaw(/*force=*/false);
}

std::uint64_t RecordedSource::wallClockNanosAt(double t) const
{
    if (impl_->t_begin_ns == 0)
    {
        return 0;
    }
    const double clamped = std::clamp(t, 0.0, impl_->duration());
    return impl_->t_begin_ns + static_cast<std::uint64_t>(clamped * kNanosPerSecond);
}

std::size_t RecordedSource::decodesPending() const
{
    const std::lock_guard<std::mutex> guard(impl_->mutex);
    return impl_->pending;
}

}  // namespace scope
