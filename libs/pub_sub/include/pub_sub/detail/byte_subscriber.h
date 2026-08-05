#ifndef PUB_SUB_DETAIL_BYTE_SUBSCRIBER_H_
#define PUB_SUB_DETAIL_BYTE_SUBSCRIBER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pub_sub::detail
{

// What a sample carries besides its bytes.
//
// Only the encoding, and only because a subscriber checks once that the schema it
// was configured for is the one actually being published. Getting that string out
// of zenoh copies it, and paying for a copy on every sample to serve a check that
// runs on the first one would be silly -- so it is a virtual the handler calls
// when it wants it, implemented over the live zenoh::Sample.
class SampleMeta
{
  public:
    // The whole encoding string, e.g. "application/capnp;CanFrame". Use
    // schemaNameFromEncoding() to get the schema half.
    virtual std::string encoding() const = 0;

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
