#ifndef ZENOH_PUBLISHER_H_
#define ZENOH_PUBLISHER_H_

#include <memory>
#include <utility>
#include <vector>

#include "zenoh.hxx"

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/io.h>

#include "pub_sub/schema_registry.h"
#include "pub_sub/session_manager.h"

#include <string_view>

namespace pub_sub
{

// Helper that owns a zenoh::Publisher and a Cap'n Proto message builder for schema T.
// Usage:
//   zenpub::Publisher<EngineRpm> pub(session, zenoh::KeyExpr("vehicle/engine/rpm"));
//   auto b = pub.builder();
//   b.setRpm(1234);
//   pub.put();
template <typename SchemaT>
class ZenohPublisher
{
public:
    using SchemaBuilder = typename SchemaT::Builder;

    ZenohPublisher(std::string_view keyexpr) :
        zenoh_session_(pub_sub::SessionManager::getOrCreate()),
        mPublisher(std::make_unique<zenoh::Publisher>(zenoh_session_->declare_publisher(keyexpr))),
        mMessage{},
        mBuilder(mMessage.initRoot<SchemaT>())
    {
    }

    bool isValid() const
    {
        return mPublisher != nullptr;
    }

    // Non-copyable but movable
    ZenohPublisher(const ZenohPublisher&) = delete;
    ZenohPublisher& operator=(const ZenohPublisher&) = delete;
    ZenohPublisher(ZenohPublisher&&) = delete;
    ZenohPublisher& operator=(ZenohPublisher&&) = delete;

    // Access the underlying Cap'n Proto builder for ergonomic field setting.
    const SchemaBuilder& fields() const
    {
        return mBuilder;
    }

    SchemaBuilder& fields()
    {
        return mBuilder;
    }

    // Publish current message state via zenoh without specifying encoding for now.
    void put()
    {
        // Avoid extra copy: flatten to a contiguous KJ array and transfer lifetime via deleter.
        // We move the owning kj::Array into the lambda capture. Zenoh stores this callable and
        // invokes it when it's done with the payload. Destroying the lambda destroys the captured
        // kj::Array, freeing the buffer at the correct time. The uint8_t* parameter is ignored
        // because the pointer simply aliases memory owned by the captured kj::Array.
        kj::Array<capnp::word> words = capnp::messageToFlatArray(mMessage);
        auto bytesView = words.asBytes();
        uint8_t* ptr = reinterpret_cast<uint8_t*>(bytesView.begin());
        const size_t len = bytesView.size();
        auto deleter = [arr = kj::mv(words)](uint8_t* /*unused*/ ) mutable {
            // Destruction of 'arr' here releases the buffer.
        };

        auto opts = zenoh::Publisher::PutOptions::create_default();
        opts.encoding.emplace("application/capnp");
        opts.encoding->set_schema(schema_traits<SchemaT>::name);

        mPublisher->put(zenoh::Bytes(ptr, len, std::move(deleter)), std::move(opts));
    }

private:
    std::shared_ptr<zenoh::Session> zenoh_session_;
    std::unique_ptr<zenoh::Publisher> mPublisher;
    ::capnp::MallocMessageBuilder mMessage;
    SchemaBuilder mBuilder;
};

}  // namespace pub_sub

#endif // ZENOH_PUBLISHER_H_

