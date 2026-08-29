#include "scope/scope_recorder.h"

#include "bag/writer.h"

#include "pub_sub/raw_subscriber.h"
#include "pub_sub/topic_directory.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <map>
#include <vector>

namespace scope
{

namespace
{

std::uint64_t wallClockNanos()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

}  // namespace

struct ScopeRecorder::Impl
{
    // DECLARED BEFORE the subscriber, so it is destroyed AFTER it. The
    // subscriber's destructor joins in-flight callbacks; the reverse order lets
    // a callback still running write into a buffer that is being destroyed. The
    // same rule holds in KeySubscription and in nodes/bag/record.cpp, and it has
    // bitten this tree before.
    CaptureBuffer buffer;

    // What EXISTS as well as what flows, for the same reason `bag record`
    // snapshots it: a topic advertised for the whole session and never
    // published is a fact only liveliness can supply, and one that cannot be
    // recovered from a file full of messages afterwards.
    //
    // A unique_ptr because stop() DROPS it. "Offline" means no zenoh
    // subscriptions at all, and this directory's liveliness watch is a
    // subscription -- keeping it up for the whole offline session would make
    // offline a label rather than a fact. What saveTo() needs survives in
    // `advertised` below.
    //
    // While online this duplicates LiveZenohSource's directory (two watches on
    // the same key space). Accepted: wiring the source's directory into the
    // recorder would couple the two objects for the life of both, to save one
    // near-free liveliness subscription.
    std::unique_ptr<pub_sub::TopicDirectory> directory;

    // The advertisement snapshot taken at stop(), so a capture reviewed and
    // saved offline still records its silent topics.
    std::vector<pub_sub::DirectoryEntry> advertised;

    std::atomic<std::uint64_t> received{0};

    std::unique_ptr<pub_sub::RawSubscriber> subscriber;

    Impl(std::size_t max_bytes, double max_seconds) : buffer(max_bytes, max_seconds) {}
};

ScopeRecorder::ScopeRecorder(std::size_t max_bytes, double max_seconds) :
    impl_(std::make_unique<Impl>(max_bytes, max_seconds))
{
    Impl* const impl = impl_.get();

    impl_->directory = std::make_unique<pub_sub::TopicDirectory>();

    impl_->subscriber = std::make_unique<pub_sub::RawSubscriber>(
        "**", pub_sub::RawSubscriber::InfoHandler(
                  [impl](const std::vector<std::uint8_t>& payload,
                         const pub_sub::RawSubscriber::SampleInfo& info)
                  {
                      // Taken HERE, not on whatever thread reads the buffer
                      // later. A consumer that is backing up would otherwise
                      // fold its own latency into every log_time, and the
                      // capture's timing would slew under load -- invisibly,
                      // because the result still looks like plausible data.
                      bag::QueuedMessage message;
                      message.log_time_ns = wallClockNanos();
                      message.key = std::string(info.keyexpr);
                      message.schema = std::string(info.schema_name);
                      message.origin_zid = std::string(info.origin_zid);
                      message.publish_time_ns = info.publish_time_nanos;
                      message.payload = payload;

                      ++impl->received;

                      // Copy in and return. This runs on a zenoh RX thread and
                      // must not block: stalling one stalls the session for
                      // everything, including the liveliness traffic the signal
                      // browser reads its topic list from.
                      impl->buffer.push(std::move(message));
                  }));

    if (!impl_->subscriber->isValid())
    {
        SPDLOG_ERROR("Could not subscribe to '**'; scope will run without a capture.");
    }
}

ScopeRecorder::~ScopeRecorder() = default;

bool ScopeRecorder::isValid() const
{
    return impl_->subscriber && impl_->subscriber->isValid();
}

void ScopeRecorder::stop()
{
    // Snapshot what liveliness knew BEFORE dropping the watch: saveTo() records
    // silent topics from this, and after the directory is gone there is nothing
    // to ask.
    if (impl_->directory != nullptr)
    {
        impl_->advertised = impl_->directory->snapshot();
        impl_->directory.reset();
    }

    // The subscriber goes too. Its destructor joins the in-flight callbacks, so
    // after this line nothing is pushing into `buffer` -- which is what makes it
    // safe for a CaptureProvider to be handed a pointer to it and read from the
    // GUI thread without the mutex being contended by an RX thread that no
    // longer exists. The BUFFER stays; see the header.
    impl_->subscriber.reset();
}

CaptureBuffer& ScopeRecorder::buffer()
{
    return impl_->buffer;
}

const CaptureBuffer& ScopeRecorder::buffer() const
{
    return impl_->buffer;
}

std::uint64_t ScopeRecorder::received() const
{
    return impl_->received.load();
}

bool ScopeRecorder::saveTo(const std::string& directory) const
{
    bag::WriterOptions options;
    options.recorder = "redline scope";

    bag::BagWriter writer(directory, options);
    if (!writer.isValid())
    {
        SPDLOG_ERROR("Could not open '{}' for writing.", directory);
        return false;
    }

    // Advertisements first, so a topic that was advertised for the whole
    // session and never published is recorded as silent rather than absent.
    // Live from the directory while capturing; from the stop() snapshot after.
    const std::vector<pub_sub::DirectoryEntry> advertised =
        impl_->directory != nullptr ? impl_->directory->snapshot() : impl_->advertised;
    for (const pub_sub::DirectoryEntry& entry : advertised)
    {
        writer.noteAdvertised(entry.key, entry.schema);
    }

    bool ok = true;
    impl_->buffer.forEach(0, std::numeric_limits<std::uint64_t>::max(),
                          [&](const bag::QueuedMessage& message)
                          {
                              ok = writer.write(message.key, message.schema, message.payload,
                                                message.log_time_ns, message.publish_time_ns,
                                                message.origin_zid);
                              // Stop at the first failed write -- the rest
                              // would fail the same way.
                              return ok;
                          });

    // Recorded as the bag's dropped_messages, because from the file's point of
    // view that is exactly what an evicted message is: a message that was on the
    // bus and is not in this recording. A saved capture that claimed to be
    // complete would be the same lie a recorder dropping samples silently tells.
    writer.noteDropped(impl_->buffer.evicted());

    return writer.close() && ok;
}

// ------------------------------------------------------------- CaptureProvider

CaptureProvider::CaptureProvider(const CaptureBuffer& buffer) : buffer_(&buffer)
{
}

void CaptureProvider::forEach(std::uint64_t t0_ns, std::uint64_t t1_ns,
                              const std::function<bool(const bag::BagMessage&)>& visit)
{
    buffer_->forEach(t0_ns, t1_ns,
                     [&visit](const bag::QueuedMessage& queued)
                     {
                         // A view over the buffer's own storage, valid only for
                         // this call -- the same contract BagReader's callback
                         // has, which is what lets one RecordedSource sit over
                         // both without knowing which it has.
                         bag::BagMessage message;
                         message.key = queued.key;
                         message.schema = queued.schema;
                         message.payload = queued.payload;
                         message.log_time_ns = queued.log_time_ns;
                         message.publish_time_ns =
                             queued.publish_time_ns.value_or(queued.log_time_ns);
                         return visit(message);
                     });
}

std::vector<TopicInfo> CaptureProvider::topics() const
{
    // Derived from what is retained, not from a directory: a topic whose
    // messages have all been evicted is genuinely no longer reviewable, and
    // listing it would offer a binding that can only produce an empty trace.
    std::map<std::string, std::string> by_key;
    buffer_->forEach(0, std::numeric_limits<std::uint64_t>::max(),
                     [&by_key](const bag::QueuedMessage& message)
                     {
                         by_key.emplace(message.key, message.schema);
                         return true;
                     });

    std::vector<TopicInfo> out;
    out.reserve(by_key.size());
    for (const auto& [key, schema] : by_key)
    {
        out.push_back(TopicInfo{key, schema, true});
    }
    return out;
}

std::pair<std::uint64_t, std::uint64_t> CaptureProvider::spanNanos() const
{
    return buffer_->spanNanos();
}

std::uint64_t CaptureProvider::revision() const
{
    return buffer_->revision();
}

bool CaptureProvider::density(std::uint64_t t0_ns, std::uint64_t t1_ns, std::size_t buckets,
                              std::vector<std::uint32_t>& out)
{
    buffer_->density(t0_ns, t1_ns, buckets, out);
    return true;
}

}  // namespace scope
