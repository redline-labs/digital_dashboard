#ifndef ZENOH_SUBSCRIBER_H_
#define ZENOH_SUBSCRIBER_H_

// ZenohTypedSubscriber, plus ZenohExpressionSubscriber for whoever already
// included this header expecting both.
//
// The two are split because their costs differ. The typed subscriber is a
// template over a capnp schema, so it needs capnp/serialize.h and
// capnp_payload.h in the header -- about 26,000 preprocessed lines. The
// expression subscriber needs none of that any more (see expression_subscriber.h,
// where it lives), and the ~27 widget translation units that use only it should
// not pay for the other one. They reach it through
// dashboard/expression_subscription.h, which includes the lean header directly.

#include "pub_sub/capnp_encoding.h"
#include "pub_sub/detail/byte_subscriber.h"
#include "pub_sub/expression_subscriber.h"
#include "pub_sub/typed_decode.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pub_sub
{

// Subscribes to a key and hands each sample to a callback as a typed capnp
// Reader. For consumers that want the whole message rather than one number out of
// it.
//
// The Reader is only valid for the duration of the callback -- it points into a
// buffer this owns, which is released as soon as the callback returns. Copy out
// what you need.
//
// The callback runs on a zenoh RX thread. It may throw; the exception is caught
// and logged rather than crossing back into zenoh. A sample of another schema or
// revision, or one capnp cannot read, is dropped and said once -- see
// SampleGate.
//
// zenoh is not in this header -- see detail::ByteSubscriber. capnp is, because
// SchemaT is the contract callers write against.
template <typename SchemaT>
class ZenohTypedSubscriber
{
  public:
    using Reader = typename SchemaT::Reader;

    ZenohTypedSubscriber(const std::string& zenoh_key, std::function<void(Reader)> on_message) :
        gate_(zenoh_key),
        subscriber_(zenoh_key,
                    [this, cb = std::move(on_message)](const std::vector<std::uint8_t>& bytes,
                                                       const detail::SampleMeta& meta) {
                        // The name is judged on the first sample only: encoding()
                        // copies a string out of zenoh, and a key does not change
                        // schema mid-stream.
                        if (!name_ok_)
                        {
                            name_ok_ = gate_.admit(schemaNameFromEncoding(meta.encoding()),
                                                   std::nullopt);
                        }
                        if (!*name_ok_ || !gate_.admitRevision(meta.layout()))
                        {
                            return;
                        }
                        decodeAs<SchemaT>(gate_, bytes, cb);
                    })
    {
    }

    bool isValid() const { return subscriber_.isValid(); }

    std::string_view keyexpr() const { return subscriber_.keyexpr(); }

  private:
    TypedGate<SchemaT> gate_;
    // Written and read only on the RX thread, which ByteSubscriber serialises.
    std::optional<bool> name_ok_;
    // Last: undeclaring joins the callback, which uses everything above.
    detail::ByteSubscriber subscriber_;
};

}  // namespace pub_sub

#endif // ZENOH_SUBSCRIBER_H_
