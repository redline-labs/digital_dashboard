#ifndef ZENOH_ASYNC_CLIENT_H_
#define ZENOH_ASYNC_CLIENT_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <capnp/message.h>
#include <capnp/serialize.h>

#include "pub_sub/detail/byte_query.h"
#include "pub_sub/typed_decode.h"

namespace pub_sub
{

// Cap'n Proto-backed GET client that does not block.
//
// ZenohClient is the same thing parking the calling thread until a reply or the
// timeout, which is right for a CLI tool and wrong for anything with a UI or an
// event loop. This one hands the reply to a callback on a zenoh thread instead.
//
// THE CALLBACK RUNS ON A ZENOH THREAD, not the caller's. It must not block and
// must not touch Qt objects -- hop with QMetaObject::invokeMethod, or post to
// whatever run loop owns the data. It fires EXACTLY ONCE per request: on the
// first ok reply, or on completion with nothing usable. Both the reply and the
// timeout arrive on zenoh's side, so "exactly once" is enforced with a flag
// rather than assumed.
//
// Each request owns its own message builder and its own state, so requests may
// overlap and destroying the client does not cancel or corrupt one in flight --
// the callback still fires. A callback that captures something the caller then
// destroys is the caller's problem, as everywhere else in this tree.
template <typename RequestT, typename ResponseT>
class ZenohAsyncClient
{
  public:
    using ResponseReader = typename ResponseT::Reader;
    using RequestBuilder = typename RequestT::Builder;

    enum class Status
    {
        // A reply arrived and decoded. `response` is non-null.
        Ok,
        // The query completed with no reply at all: nobody is serving that key,
        // or nobody answered inside the timeout. Indistinguishable on the wire,
        // and the caller's response is the same either way.
        NoReply,
        // Something replied, and it was another schema or revision, not a
        // whole number of capnp words, or not readable. Each would otherwise
        // decode as a plausible answer rather than a failure.
        Malformed,
        // The get() call itself could not be made.
        Failed,
    };

    static const char* to_string(Status status)
    {
        switch (status)
        {
            case Status::Ok:
                return "ok";
            case Status::NoReply:
                return "no reply";
            case Status::Malformed:
                return "malformed reply";
            case Status::Failed:
                return "request failed";
        }
        return "unknown";
    }

    ZenohAsyncClient(std::string keyexpr, std::uint64_t timeoutMs) :
        mKeyExpr(std::move(keyexpr)),
        mTimeoutMs(timeoutMs),
        mGate(std::make_shared<TypedGate<ResponseT>>(mKeyExpr))
    {
    }

    ZenohAsyncClient(const ZenohAsyncClient&) = delete;
    ZenohAsyncClient& operator=(const ZenohAsyncClient&) = delete;

    const std::string& key() const { return mKeyExpr; }

    // `fill(RequestBuilder&)` writes the request; `on_reply(Status, const
    // ResponseReader*)` gets the outcome, once. Returns false when the request
    // could not be sent, having already called `on_reply` with Failed.
    template <typename Fill, typename Callback>
    bool request(Fill&& fill, Callback&& on_reply)
    {
        auto state = std::make_shared<Pending>(std::forward<Callback>(on_reply));

        capnp::MallocMessageBuilder message;
        auto root = message.template initRoot<RequestT>();
        fill(root);
        const kj::Array<capnp::word> words = capnp::messageToFlatArray(message);
        const kj::ArrayPtr<const kj::byte> bytes = words.asBytes();

        // The gate by value: a reply may arrive after this client is gone.
        const std::shared_ptr<TypedGate<ResponseT>> gate = mGate;
        const bool sent = detail::queryBytes(
            mKeyExpr, schema_traits<RequestT>::name, schema_traits<RequestT>::layout,
            std::vector<std::uint8_t>(bytes.begin(), bytes.end()), mTimeoutMs,
            [state, gate](const detail::ByteMessage& reply) {
                if (state->delivered.load(std::memory_order_acquire))
                {
                    return;
                }
                if (!gate->admit(reply.schema_name, reply.layout) ||
                    !decodeAs<ResponseT>(*gate, reply.payload, [&](ResponseReader response) {
                        state->deliver(Status::Ok, &response);
                    }))
                {
                    state->deliver(Status::Malformed, nullptr);
                }
            },
            [state] { state->deliver(Status::NoReply, nullptr); });
        if (!sent)
        {
            state->deliver(Status::Failed, nullptr);
        }
        return sent;
    }

  private:
    struct Pending
    {
        template <typename Callback>
        explicit Pending(Callback&& callback) : handler(std::forward<Callback>(callback))
        {
        }

        void deliver(Status status, const ResponseReader* response)
        {
            if (delivered.exchange(true, std::memory_order_acq_rel))
            {
                return;
            }
            if (handler)
            {
                handler(status, response);
            }
        }

        std::function<void(Status, const ResponseReader*)> handler;
        std::atomic<bool> delivered { false };
    };

    std::string mKeyExpr;
    std::uint64_t mTimeoutMs;
    std::shared_ptr<TypedGate<ResponseT>> mGate;
};

}  // namespace pub_sub

#endif  // ZENOH_ASYNC_CLIENT_H_
