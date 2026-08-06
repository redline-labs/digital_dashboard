#ifndef PUB_SUB_RAW_SUBSCRIBER_H_
#define PUB_SUB_RAW_SUBSCRIBER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pub_sub
{

// A subscription that hands back payload bytes and the schema name the
// publisher stamped on them, and decodes nothing itself.
//
// This is the seam for a consumer that does not know at compile time what it
// will be handed, or that wants to decode one sample several ways. The scope
// app is both: it subscribes once per zenoh key and evaluates however many
// expressions are bound to that key against each sample, rather than opening
// one subscription per plotted signal and decoding the same message N times.
//
// It is a thin public wrapper over detail::ByteSubscriber -- deliberately, so
// that "the seam, not the API" stays true of the detail header while consumers
// outside pub_sub still have a supported way in. What it adds is splitting the
// schema name out of the encoding eagerly, which is what callers want and which
// otherwise means every one of them reaching for capnp_encoding.h.
class RawSubscriber
{
  public:
    // `schema_name` is the half after the ';' of "application/capnp;EngineRpm",
    // and is empty when the publisher named no schema. It is a view over a
    // string owned for the duration of the call only -- copy it to keep it.
    //
    // Runs on a zenoh RX thread. Must not block and must not throw: the frame
    // above is Rust, and an exception crossing that boundary aborts the process.
    // RawSubscriber catches anything that escapes anyway, because "must not" is
    // not "cannot".
    using Handler = std::function<void(const std::vector<std::uint8_t>& payload,
                                       std::string_view schema_name)>;

    // Everything about a sample except its bytes, resolved eagerly.
    //
    // The reason this exists alongside Handler: a subscriber on a wildcard gets
    // one callback per key and the plain Handler does not say which. Scope binds
    // one concrete key per subscription so it never needed to know; a bag
    // recorder subscribing to "**", or `inspect watch` tabulating the whole bus,
    // cannot work without it.
    //
    // Every field is a view or a scalar over storage owned for the duration of
    // the call. Copy anything you keep.
    struct SampleInfo
    {
        // The key this sample was published on -- the concrete one, never a
        // wildcard.
        std::string_view keyexpr;

        // The half after the ';' of "application/capnp;EngineRpm". Empty when
        // the publisher named no schema.
        std::string_view schema_name;

        // When the publisher's session stamped it, nanoseconds since the UNIX
        // epoch. Empty when the sample arrived unstamped, which a consumer must
        // handle rather than assume away -- see detail::SampleMeta.
        std::optional<std::uint64_t> publish_time_nanos;

        // The session that stamped it. Empty when unstamped.
        //
        // Resolved eagerly like the rest, which costs a string copy per sample.
        // That is the one deliberate cost in this struct, and it is here because
        // the consumers that want SampleInfo at all -- a recorder, a bus monitor
        // -- all want the origin, so making it lazy would just move the copy.
        std::string_view origin_zid;
    };

    using InfoHandler =
        std::function<void(const std::vector<std::uint8_t>& payload, const SampleInfo& info)>;

    RawSubscriber(const std::string& keyexpr, Handler on_sample);

    // The same subscription, with the fuller per-sample information above.
    //
    // Not a default argument or a flag on the first constructor: the two take
    // different callbacks, and which one a caller passes is exactly how it says
    // what it needs. A caller that only wants the schema name should not pay for
    // the origin zid.
    RawSubscriber(const std::string& keyexpr, InfoHandler on_sample);

    // Undeclares the subscription, which joins any in-flight callback -- so once
    // this returns, `on_sample` is guaranteed not to be running. Everything the
    // handler captures must therefore outlive this object: declare it last.
    ~RawSubscriber();

    RawSubscriber(const RawSubscriber&) = delete;
    RawSubscriber& operator=(const RawSubscriber&) = delete;
    RawSubscriber(RawSubscriber&&) = delete;
    RawSubscriber& operator=(RawSubscriber&&) = delete;

    // False when the subscription could not be declared. A consumer that
    // ignores this gets an object that never delivers anything and never says
    // why.
    bool isValid() const;

    std::string_view keyexpr() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pub_sub

#endif  // PUB_SUB_RAW_SUBSCRIBER_H_
