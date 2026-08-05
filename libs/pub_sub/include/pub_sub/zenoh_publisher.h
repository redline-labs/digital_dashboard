#ifndef ZENOH_PUBLISHER_H_
#define ZENOH_PUBLISHER_H_

#include <capnp/message.h>
#include <capnp/serialize.h>

#include "pub_sub/detail/byte_publisher.h"
#include "pub_sub/schema_registry.h"

#include <cstring>
#include <functional>
#include <new>
#include <string_view>
#include <utility>

namespace pub_sub
{

// Owns a Cap'n Proto message builder for schema T and publishes it on a key.
//
//   pub_sub::ZenohPublisher<EngineRpm> pub("vehicle/engine/rpm");
//   pub.fields().setRpm(1234);
//   pub.put();
//
// NOT thread-safe, and deliberately so: there is one builder inside, and two
// threads calling fields() are writing into the same message. Give each thread
// its own publisher, or hold a lock across fields()+put() -- which is what
// nodes/carplay/zenoh_bridge.h does, one mutex per publisher.
//
// Note also that put() re-roots the builder, so a reference from fields() held
// across a put() refers to the *next* message, not the one just sent:
//
//     auto& f = pub.fields();
//     f.setRpm(1);  pub.put();   // publishes rpm=1
//     f.setRpm(2);  pub.put();   // publishes rpm=2, not rpm=1 amended
//
// which is usually what you wanted, but not if you expected to read back what
// you just published.
//
// zenoh itself is not in this header -- see detail::BytePublisher. capnp is,
// because SchemaT is the contract callers write against.
template <typename SchemaT>
class ZenohPublisher
{
public:
    using SchemaBuilder = typename SchemaT::Builder;

    explicit ZenohPublisher(std::string_view keyexpr) :
        mPublisher(keyexpr, schema_traits<SchemaT>::name),
        mScratch(zeroedScratch(kInitialScratchWords)),
        mMessage(mScratch, ::capnp::AllocationStrategy::GROW_HEURISTICALLY),
        mBuilder(mMessage.initRoot<SchemaT>())
    {
    }

    ZenohPublisher(const ZenohPublisher&) = delete;
    ZenohPublisher& operator=(const ZenohPublisher&) = delete;
    ZenohPublisher(ZenohPublisher&&) = delete;
    ZenohPublisher& operator=(ZenohPublisher&&) = delete;

    bool isValid() const { return mPublisher.isValid(); }

    // The Cap'n Proto builder for the message being assembled. Raw on purpose:
    // the generated setters *are* the API for a schema, and wrapping them would
    // mean regenerating a facade for every field of every schema.
    const SchemaBuilder& fields() const { return mBuilder; }
    SchemaBuilder& fields() { return mBuilder; }

    std::string_view keyexpr() const { return mPublisher.keyexpr(); }

    // Reports whether anything is subscribed to this key, and calls `handler`
    // whenever that changes. Useful for work only worth doing when someone is
    // listening -- zenoh has no retained messages, so a publisher that has to
    // prime a late joiner otherwise has to do it on a timer, forever, whether
    // or not anyone ever connects.
    //
    // NOTE the granularity: zenoh reports a *boolean*, so the handler fires on
    // the first subscriber arriving and the last one leaving, and NOT for a
    // second subscriber joining while a first is still there. It answers "is
    // anyone listening", not "how many" and not "who just arrived". Anything
    // that has to serve a late joiner arriving alongside an existing one still
    // needs its own periodic path.
    //
    // The handler runs on a zenoh thread, so it must not block. The listener
    // lives as long as the publisher.
    void onSubscriberPresenceChanged(std::function<void(bool present)> handler)
    {
        mPublisher.onSubscriberPresenceChanged(std::move(handler));
    }

    bool hasSubscribers() const { return mPublisher.hasSubscribers(); }

    // Serialise the current message and publish it, then start a fresh one.
    void put()
    {
        kj::Array<capnp::word> words = capnp::messageToFlatArray(mMessage);
        const size_t flat_words = words.size();

        // Ownership goes with it: zenoh may hold the payload after this returns,
        // which is why the buffer cannot be pooled and this is the one allocation
        // per message that has to stay.
        mPublisher.put(kj::mv(words));

        rebuildOverScratch(flat_words);
    }

private:
    // First-segment size for the message builder, in 8-byte words. Small enough
    // to be free, and grown to fit on the first message that needs more -- see
    // rebuildOverScratch(). 64 words covers every telemetry schema in this tree;
    // the CarPlay video and audio publishers outgrow it once and then settle.
    static constexpr size_t kInitialScratchWords = 64;

    // capnp requires a scratch first segment to be zeroed, and checks the first
    // word to catch the mistake. MallocMessageBuilder's destructor re-zeroes
    // exactly the words it used, so this is only needed when the buffer is new.
    static kj::Array<capnp::word> zeroedScratch(size_t words)
    {
        kj::Array<capnp::word> scratch = kj::heapArray<capnp::word>(words);
        std::memset(scratch.begin(), 0, scratch.size() * sizeof(capnp::word));
        return scratch;
    }

    // Re-root the builder for the next message.
    //
    // The builder has to be reset or its arena grows without bound, and
    // MallocMessageBuilder is neither copyable nor movable, so it is destroyed
    // and reconstructed in place. What matters is what it is reconstructed
    // *over*: a default-constructed MallocMessageBuilder callocs a fresh first
    // segment for every message (message.c++ allocateSegment), and zeroing that
    // segment cost real time -- 554 ns per put() against 427 with this. capnp
    // sanctions the reuse explicitly ("useful when building lots of small
    // messages in a tight loop", message.h), which is why its destructor bothers
    // to re-zero the segment it used.
    //
    // `flat_words` is what the message just serialised to. Growing the scratch to
    // match means a publisher whose messages are bigger than the default pays one
    // reallocation, on its first message, instead of spilling into freshly
    // calloc'd extra segments forever.
    void rebuildOverScratch(size_t flat_words)
    {
        mMessage.~MallocMessageBuilder();
        if (flat_words > mScratch.size())
        {
            mScratch = zeroedScratch(flat_words);
        }
        new (&mMessage) ::capnp::MallocMessageBuilder(
            mScratch, ::capnp::AllocationStrategy::GROW_HEURISTICALLY);
        mBuilder = mMessage.initRoot<SchemaT>();
    }

    detail::BytePublisher mPublisher;

    // Declared before mMessage: the builder holds a pointer into it, so it has to
    // outlive every builder constructed over it and be destroyed after the last.
    kj::Array<capnp::word> mScratch;
    ::capnp::MallocMessageBuilder mMessage;
    SchemaBuilder mBuilder;
};

}  // namespace pub_sub

#endif // ZENOH_PUBLISHER_H_
