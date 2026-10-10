#ifndef ZENOH_CLIENT_H_
#define ZENOH_CLIENT_H_

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <capnp/message.h>
#include <capnp/serialize.h>

#include "pub_sub/detail/byte_query.h"
#include "pub_sub/typed_decode.h"

namespace pub_sub
{

// A Cap'n Proto request/reply client that blocks: request() parks the calling
// thread until the first reply or the timeout, then hands the reply to
// `on_response` on that same thread. Right for a CLI tool; see ZenohAsyncClient
// for anything with an event loop.
//
// The request is stamped with its schema and revision; a reply of another
// schema or revision is refused, exactly as a subscriber refuses a sample.
template <typename RequestT, typename ResponseT>
class ZenohClient
{
  public:
    using RequestReader = typename RequestT::Reader;
    using ResponseReader = typename ResponseT::Reader;

    ZenohClient(const std::string& keyexpr, uint64_t timeoutMs) :
        mRequest(mBuilder.template initRoot<RequestT>()),
        mKeyExpr(keyexpr),
        mTimeoutMs(timeoutMs),
        mGate(std::make_shared<TypedGate<ResponseT>>(keyexpr))
    {
    }

    typename RequestT::Builder& fields() { return mRequest; }

    // True once `on_response` has been called with a reply.
    template <typename Handler>
    bool request(Handler&& on_response)
    {
        const kj::Array<capnp::word> words = capnp::messageToFlatArray(mBuilder);
        const kj::ArrayPtr<const kj::byte> bytes = words.asBytes();

        struct Wait
        {
            std::mutex mutex;
            std::condition_variable changed;
            std::optional<detail::ByteMessage> reply;
            bool done = false;
        };
        auto wait = std::make_shared<Wait>();

        const bool sent = detail::queryBytes(
            mKeyExpr, schema_traits<RequestT>::name, schema_traits<RequestT>::layout,
            std::vector<std::uint8_t>(bytes.begin(), bytes.end()), mTimeoutMs,
            [wait](const detail::ByteMessage& reply) {
                const std::lock_guard<std::mutex> lock(wait->mutex);
                if (!wait->reply && !reply.payload.empty())
                {
                    wait->reply = reply;
                    wait->changed.notify_all();
                }
            },
            [wait] {
                const std::lock_guard<std::mutex> lock(wait->mutex);
                wait->done = true;
                wait->changed.notify_all();
            });
        if (!sent)
        {
            return false;
        }

        std::unique_lock<std::mutex> lock(wait->mutex);
        wait->changed.wait(lock, [&wait] { return wait->reply.has_value() || wait->done; });
        if (!wait->reply)
        {
            return false;
        }
        const detail::ByteMessage reply = std::move(*wait->reply);
        lock.unlock();

        if (!mGate->admit(reply.schema_name, reply.layout))
        {
            return false;
        }
        return decodeAs<ResponseT>(*mGate, reply.payload,
                                   [&](ResponseReader response) { on_response(response); });
    }

  private:
    capnp::MallocMessageBuilder mBuilder;
    typename RequestT::Builder mRequest;
    std::string mKeyExpr;
    uint64_t mTimeoutMs;
    std::shared_ptr<TypedGate<ResponseT>> mGate;
};

}  // namespace pub_sub

#endif  // ZENOH_CLIENT_H_
