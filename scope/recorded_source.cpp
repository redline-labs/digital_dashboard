#include "scope/recorded_source.h"

#include "pub_sub/expression_evaluator.h"

#include <reflection/reflection.h>
#include <spdlog/spdlog.h>

#include <algorithm>
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
                              const std::function<void(const bag::BagMessage&)>& visit)
{
    if (!reader_->isValid())
    {
        return;
    }

    reader_->forEach(t0_ns, t1_ns,
                     [&visit](const bag::BagMessage& message)
                     {
                         visit(message);
                         return true;
                     });
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

    // ONE worker thread, not one per bind. RecordedProvider is not thread-safe
    // -- BagFileProvider holds an mcap reader with its own decompression buffers
    // -- so decodes are serialized by construction rather than by a lock nobody
    // would remember to take.
    std::deque<std::shared_ptr<RecordedBinding>> queue;
    std::condition_variable work;
    bool stopping = false;
    std::size_t pending = 0;
    std::thread worker;

    double duration() const
    {
        return t_end_ns > t_begin_ns
                   ? static_cast<double>(t_end_ns - t_begin_ns) / kNanosPerSecond
                   : 0.0;
    }

    // The full decode pass for one signal. Runs on the worker thread.
    void decode(const std::shared_ptr<RecordedBinding>& binding)
    {
        std::vector<Sample> decoded;

        provider->forEach(
            t_begin_ns, t_end_ns,
            [&](const bag::BagMessage& message)
            {
                if (message.key != binding->key.zenoh_key)
                {
                    return;
                }

                // Recorded with a different schema than the binding expects.
                // Skipped rather than decoded: capnp will happily read these
                // bytes against the wrong schema and produce a number.
                if (!message.schema.empty() && message.schema != binding->expected_schema)
                {
                    return;
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

                // A span, so nothing is copied. nullopt drops the sample rather
                // than pushing zero: a gap in the line is honest, a spike that
                // never happened is not.
                if (const std::optional<double> value =
                        binding->evaluator->evaluateToDouble(message.payload))
                {
                    decoded.push_back(Sample{t, *value});
                }
            });

        const std::lock_guard<std::mutex> guard(mutex);
        binding->samples = std::move(decoded);
        binding->ready = true;

        // Cleared so the next refill rebuilds the window from scratch. The
        // buffer is empty at this point and `filled_to` describes a vector that
        // did not exist when it was last set.
        binding->filled = false;
        binding->filled_to = 0;
    }

    void run()
    {
        for (;;)
        {
            std::shared_ptr<RecordedBinding> job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                work.wait(lock, [this]() { return stopping || !queue.empty(); });
                if (stopping)
                {
                    return;
                }
                job = queue.front();
                queue.pop_front();
            }

            decode(job);

            {
                const std::lock_guard<std::mutex> guard(mutex);
                --pending;
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
        impl_->queue.push_back(binding);
        ++impl_->pending;
    }
    impl_->work.notify_one();

    SPDLOG_DEBUG("Bound recorded signal {} to '{}' ({}), expression '{}'; decoding.",
                 binding->handle, key.zenoh_key, reflection::enum_to_string(key.schema_type),
                 key.value_expression);
    return binding->handle;
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
    impl_->queue.erase(std::remove(impl_->queue.begin(), impl_->queue.end(), found->second),
                       impl_->queue.end());

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
    // a buffer to fill, and it has no other way to say so.
    impl_->refillAll(/*force=*/false);
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
