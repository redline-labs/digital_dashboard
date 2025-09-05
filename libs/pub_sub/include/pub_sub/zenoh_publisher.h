#ifndef ZENOH_PUBLISHER_H_
#define ZENOH_PUBLISHER_H_

#include <memory>
#include <utility>
#include <vector>

#include "zenoh.hxx"

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/io.h>

#include "pub_sub/session_manager.h"

#include <string_view>

namespace zenoh_publisher
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
        zenoh_session_(zenoh_session_manager::SessionManager::getOrCreate()),
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
        kj::VectorOutputStream stream;
        capnp::writeMessage(stream, mMessage);
        auto data = stream.getArray();
        std::vector<uint8_t> bytes(data.begin(), data.end());
        mPublisher->put(zenoh::Bytes(std::move(bytes)));
    }

private:
    std::shared_ptr<zenoh::Session> zenoh_session_;
    std::unique_ptr<zenoh::Publisher> mPublisher;
    ::capnp::MallocMessageBuilder mMessage;
    SchemaBuilder mBuilder;
    
};

}  // namespace zenoh_publisher

#endif // ZENOH_PUBLISHER_H_

