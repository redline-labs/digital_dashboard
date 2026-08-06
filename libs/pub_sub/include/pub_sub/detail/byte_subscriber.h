#ifndef PUB_SUB_DETAIL_BYTE_SUBSCRIBER_H_
#define PUB_SUB_DETAIL_BYTE_SUBSCRIBER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pub_sub::detail
{

// What a sample carries besides its bytes.
//
// Every accessor here is a virtual the handler calls only when it wants the
// answer, rather than a field filled in for every sample. That shape is not
// stylistic: encoding() and originZid() each copy a string out of zenoh, and a
// subscriber typically checks the encoding once (on the first sample, to confirm
// the schema is the one it was configured for) and never looks again. Paying for
// those copies on every sample to serve a check that runs once would be silly.
//
// Implemented over the live zenoh::Sample, so the object is only valid for the
// duration of the handler call.
class SampleMeta
{
  public:
    // The whole encoding string, e.g. "application/capnp;CanFrame". Use
    // schemaNameFromEncoding() to get the schema half.
    virtual std::string encoding() const = 0;

    // The key this sample was actually published on.
    //
    // NOT the same as the subscription's key expression whenever that contains a
    // wildcard: a subscriber on "**" gets one callback per key and has no other
    // way to tell them apart. The view borrows from the live sample, so copy it
    // to keep it.
    virtual std::string_view keyexpr() const = 0;

    // When the publisher's session stamped this sample, in nanoseconds since the
    // UNIX epoch -- or nullopt when it arrived unstamped.
    //
    // Unstamped is a real case, not a defensive one: sessions only stamp because
    // SessionManager turns timestamping on, so anything publishing through a
    // differently configured session (or an older build) has no time at all. A
    // consumer that needs a time for every sample must supply its own for these
    // and say how many it had to.
    //
    // See pub_sub/timestamp.h for what this clock does and does not promise --
    // in particular that it is the publisher's wall clock, so it can be wrong.
    virtual std::optional<std::uint64_t> publishTimeNanos() const = 0;

    // The zenoh session id that stamped the sample: who actually sent it, as
    // opposed to who advertises the topic. Empty when the sample is unstamped.
    //
    // Copies, like encoding(). This is the data-path counterpart to the zid an
    // advertisement carries, and the two agreeing is what confirms a topic is
    // being published by the node that claims it.
    virtual std::string originZid() const = 0;

  protected:
    ~SampleMeta() = default;
};

// The zenoh half of a subscriber, with no zenoh in the header.
//
// Same reasoning as BytePublisher: the classes built on this are templated on a
// capnp schema and so must live in headers, and <zenoh.hxx> is 89,000
// preprocessed lines that every such translation unit would otherwise pay. What
// zenoh actually does here -- declare a subscription, hand back payload bytes --
// is not templated.
//
// Nothing outside pub_sub should name this type; it is the seam, not the API.
class ByteSubscriber
{
  public:
    // Runs on a zenoh RX thread. Must not throw: the frame above it is Rust, and
    // an exception crossing that boundary aborts the process. ByteSubscriber
    // catches anything that escapes anyway, because "must not" is not "cannot".
    using Handler = std::function<void(const std::vector<std::uint8_t>& payload,
                                       const SampleMeta& meta)>;

    ByteSubscriber(const std::string& keyexpr, Handler on_sample);

    // Undeclares the subscription, which joins any in-flight callback -- so once
    // this returns, `on_sample` is guaranteed not to be running. Everything the
    // handler captures must therefore outlive this object: declare it last.
    ~ByteSubscriber();

    ByteSubscriber(const ByteSubscriber&) = delete;
    ByteSubscriber& operator=(const ByteSubscriber&) = delete;
    ByteSubscriber(ByteSubscriber&&) = delete;
    ByteSubscriber& operator=(ByteSubscriber&&) = delete;

    bool isValid() const;

    std::string_view keyexpr() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Logs "payload is not a whole number of capnp words" for `keyexpr`.
//
// A free function purely so the templated subscribers that need it do not have to
// include spdlog -- which is the same reasoning as the rest of this header, and
// worth 16,000 preprocessed lines per translation unit.
void warnPartialWordPayload(std::string_view keyexpr, std::size_t bytes);

}  // namespace pub_sub::detail

#endif  // PUB_SUB_DETAIL_BYTE_SUBSCRIBER_H_
