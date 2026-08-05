#ifndef PUB_SUB_DETAIL_BYTE_PUBLISHER_H_
#define PUB_SUB_DETAIL_BYTE_PUBLISHER_H_

#include <capnp/common.h>

#include <kj/array.h>

#include <functional>
#include <memory>
#include <string_view>

namespace pub_sub::detail
{

// The zenoh half of a publisher, with no zenoh in the header.
//
// ZenohPublisher<SchemaT> has to stay a template -- the capnp schema type *is*
// the contract its callers write against -- so its definition is in a header,
// and everything that header includes is paid for by every translation unit that
// touches a publisher. Including <zenoh.hxx> there cost 89,000 preprocessed
// lines per TU for a class whose zenoh surface is "declare a publisher, put
// bytes on it, tell me if anyone is listening". That surface is not templated,
// so it lives here instead, behind a pointer.
//
// Nothing outside pub_sub should name this type; it is the seam, not the API.
class BytePublisher
{
  public:
    // `schema_name` is stamped on every sample's encoding, which is what lets a
    // subscriber identify a topic from the first message it happens to catch.
    BytePublisher(std::string_view keyexpr, std::string_view schema_name);
    ~BytePublisher();

    BytePublisher(const BytePublisher&) = delete;
    BytePublisher& operator=(const BytePublisher&) = delete;
    BytePublisher(BytePublisher&&) = delete;
    BytePublisher& operator=(BytePublisher&&) = delete;

    bool isValid() const;

    std::string_view keyexpr() const;

    // Takes ownership of `payload` and hands zenoh a view of it, releasing it
    // when zenoh is done. That transfer is the reason this takes the array by
    // value rather than a pointer and length: zenoh may hold the buffer after
    // this returns, so the caller cannot pool or reuse it.
    void put(kj::Array<capnp::word> payload);

    bool hasSubscribers() const;

    // Fires when the answer to hasSubscribers() changes. Runs on a zenoh thread,
    // so the handler must not block. The listener lives as long as this object.
    void onSubscriberPresenceChanged(std::function<void(bool present)> handler);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pub_sub::detail

#endif  // PUB_SUB_DETAIL_BYTE_PUBLISHER_H_
