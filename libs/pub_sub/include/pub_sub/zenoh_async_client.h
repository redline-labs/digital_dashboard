#ifndef ZENOH_ASYNC_CLIENT_H_
#define ZENOH_ASYNC_CLIENT_H_

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <zenoh.hxx>

#include <capnp/message.h>
#include <capnp/serialize.h>

#include "pub_sub/capnp_encoding.h"
#include "pub_sub/capnp_payload.h"
#include "pub_sub/schema_registry.h"
#include "pub_sub/session_manager.h"

#include "spdlog/spdlog.h"

namespace pub_sub
{

// Cap'n Proto-backed GET client that does not block.
//
// ZenohClient is the same thing with a FifoChannel and a loop around recv(),
// which is right for a CLI tool and wrong for anything with a UI or an event
// loop: it parks the calling thread until a reply or the timeout. This one hands
// the reply to a callback on a zenoh thread instead.
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
        // Something replied, and the payload was not a whole number of capnp
        // words. A truncated message would otherwise decode as a message whose
        // every field is default -- a plausible answer rather than a failure.
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
        mSession(pub_sub::SessionManager::getOrCreate())
    {
        SPDLOG_DEBUG("Async client on '{}' for schemas '{}'->'{}'", mKeyExpr,
                     schema_traits<RequestT>::name, schema_traits<ResponseT>::name);
    }

    ZenohAsyncClient(const ZenohAsyncClient&) = delete;
    ZenohAsyncClient& operator=(const ZenohAsyncClient&) = delete;

    const std::string& key() const { return mKeyExpr; }

    // Send one request.
    //
    //   fill:     void(RequestBuilder&) -- populates a builder owned by this
    //             request alone, so overlapping calls cannot share state.
    //   on_reply: void(Status, const ResponseReader*) -- called exactly once,
    //             on a zenoh thread. `response` is non-null only for Status::Ok
    //             and is valid only for the duration of the call.
    //
    // Returns false when the request could not be sent at all, in which case
    // on_reply has already been called with Status::Failed -- so a caller that
    // only handles the callback still handles every outcome.
    template <typename Fill, typename Callback>
    bool request(Fill&& fill, Callback&& on_reply)
    {
        // Shared rather than captured by value: both closures need it, and
        // whichever fires last is what releases it.
        auto state = std::make_shared<Pending>(std::forward<Callback>(on_reply));

        if (!mSession)
        {
            state->deliver(Status::Failed, nullptr);
            return false;
        }

        capnp::MallocMessageBuilder message;
        auto root = message.template initRoot<RequestT>();
        fill(root);

        const kj::Array<capnp::word> words = capnp::messageToFlatArray(message);
        const kj::ArrayPtr<const kj::byte> view = words.asBytes();
        std::vector<std::uint8_t> payload(view.size());
        std::memcpy(payload.data(), view.begin(), view.size());

        try
        {
            zenoh::Session::GetOptions options = zenoh::Session::GetOptions::create_default();
            options.timeout_ms = mTimeoutMs;
            options.payload.emplace(std::move(payload));
            options.encoding.emplace(kCapnpEncodingMime);
            options.encoding->set_schema(std::string(schema_traits<RequestT>::name));

            const std::string key = mKeyExpr;

            mSession->get(
                zenoh::KeyExpr(mKeyExpr), "",
                [state, key](const zenoh::Reply& reply) {
                    // Nothing may escape into zenoh's Rust frame.
                    try
                    {
                        if (state->delivered.load(std::memory_order_acquire))
                        {
                            // A second responder. The first answer stands; more
                            // than one node serving a key is a configuration
                            // problem, not something to resolve here.
                            return;
                        }
                        if (!reply.is_ok())
                        {
                            return;
                        }

                        const zenoh::Sample& sample = reply.get_ok();
                        const std::vector<std::uint8_t> bytes = sample.get_payload().as_vector();
                        const WordAlignedPayload aligned(bytes);
                        if (aligned.empty())
                        {
                            SPDLOG_WARN("Reply from '{}' was not a whole number of capnp words",
                                        key);
                            state->deliver(Status::Malformed, nullptr);
                            return;
                        }

                        capnp::FlatArrayMessageReader reader(aligned.words());
                        const ResponseReader response = reader.template getRoot<ResponseT>();
                        state->deliver(Status::Ok, &response);
                    }
                    catch (const std::exception& e)
                    {
                        SPDLOG_ERROR("Reply from '{}' could not be decoded: {}", key, e.what());
                        state->deliver(Status::Malformed, nullptr);
                    }
                    catch (...)
                    {
                        state->deliver(Status::Malformed, nullptr);
                    }
                },
                [state]() {
                    // The drop handler: the query is over. If nothing was
                    // delivered, nothing is coming -- this is where a timeout or
                    // an unserved key turns into an answer, so a caller never
                    // waits forever for a callback that was never going to fire.
                    state->deliver(Status::NoReply, nullptr);
                },
                std::move(options));
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("Request to '{}' failed: {}", mKeyExpr, e.what());
            state->deliver(Status::Failed, nullptr);
            return false;
        }

        return true;
    }

  private:
    // One request's worth of state, owned by the two zenoh closures.
    struct Pending
    {
        template <typename Callback>
        explicit Pending(Callback&& callback) : handler(std::forward<Callback>(callback))
        {
        }

        void deliver(Status status, const ResponseReader* response)
        {
            // exchange, not load-then-store: the reply callback and the drop
            // handler run on zenoh threads and can race, and the whole promise
            // of this class is that the caller's callback runs once.
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
    std::shared_ptr<zenoh::Session> mSession;
};

} // namespace pub_sub

#endif // ZENOH_ASYNC_CLIENT_H_
