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

#include <capnp/serialize.h>

#include "pub_sub/capnp_payload.h"
#include "pub_sub/detail/byte_subscriber.h"
#include "pub_sub/expression_subscriber.h"

#include <cstdint>
#include <functional>
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
// and logged rather than crossing back into zenoh.
//
// zenoh is not in this header -- see detail::ByteSubscriber. capnp is, because
// SchemaT is the contract callers write against.
template <typename SchemaT>
class ZenohTypedSubscriber
{
  public:
    using Reader = typename SchemaT::Reader;

    ZenohTypedSubscriber(const std::string& zenoh_key, std::function<void(Reader)> on_message) :
        subscriber_(zenoh_key,
                    [cb = std::move(on_message), key = zenoh_key](
                        const std::vector<std::uint8_t>& bytes, const detail::SampleMeta&) {
                        // A partial word cannot be a message. capnp would read the
                        // short buffer as one whose fields are all default, so a
                        // damaged packet would look like a healthy one reporting
                        // zero; refuse it instead.
                        const WordAlignedPayload aligned(bytes);
                        if (aligned.empty())
                        {
                            detail::warnPartialWordPayload(key, bytes.size());
                            return;
                        }

                        capnp::FlatArrayMessageReader reader(aligned.words());
                        cb(reader.getRoot<SchemaT>());
                    })
    {
    }

    bool isValid() const { return subscriber_.isValid(); }

    std::string_view keyexpr() const { return subscriber_.keyexpr(); }

  private:
    detail::ByteSubscriber subscriber_;
};

}  // namespace pub_sub

#endif // ZENOH_SUBSCRIBER_H_
