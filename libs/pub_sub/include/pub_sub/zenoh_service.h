#ifndef ZENOH_SERVICE_H_
#define ZENOH_SERVICE_H_

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <capnp/message.h>
#include <capnp/serialize.h>

#include "pub_sub/detail/byte_query.h"
#include "pub_sub/typed_decode.h"

namespace pub_sub
{

// A Cap'n Proto request/reply service on a zenoh key.
//
// Handler: void(const RequestT::Reader&, ResponseT::Builder&), called on a
// zenoh thread once per request. A request is judged by SampleGate first --
// another schema or revision is answered with an error rather than decoded into
// plausible wrong values -- and a handler that throws, or reads a malformed
// request, is answered with an error too. Replies carry the response schema and
// revision, so a client checks them the same way.
//
// zenoh is not in this header -- see detail::ByteQueryable. capnp is, because
// the schemas are the contract.
//
// Not copyable or movable: the queryable's callback captures `this`.
template <typename RequestT, typename ResponseT>
class ZenohService
{
  public:
    using RequestReader = typename RequestT::Reader;
    using ResponseBuilder = typename ResponseT::Builder;
    using Handler = std::function<void(const RequestReader&, ResponseBuilder&)>;

    ZenohService(std::string keyexpr, Handler handler) :
        mHandler(std::move(handler)),
        mGate(keyexpr),
        mQueryable(std::move(keyexpr), schema_traits<RequestT>::name, schema_traits<ResponseT>::name,
                   schema_traits<ResponseT>::layout,
                   [this](const detail::ByteMessage& request) { return answer(request); })
    {
    }

    ZenohService(const ZenohService&) = delete;
    ZenohService& operator=(const ZenohService&) = delete;
    ZenohService(ZenohService&&) = delete;
    ZenohService& operator=(ZenohService&&) = delete;

    bool isValid() const { return mQueryable.isValid(); }

  private:
    detail::ByteQueryable::Answer answer(const detail::ByteMessage& request)
    {
        if (!mGate.admit(request.schema_name, request.layout))
        {
            return {{}, "the request is not a " + mGate.schemaName() + " of this revision"};
        }

        capnp::MallocMessageBuilder response;
        auto builder = response.template initRoot<ResponseT>();
        const bool decoded = decodeAs<RequestT>(mGate, request.payload,
                                                [&](RequestReader reader) { mHandler(reader, builder); });
        if (!decoded)
        {
            return {{}, "the request could not be read as a " + mGate.schemaName()};
        }

        const kj::Array<capnp::word> words = capnp::messageToFlatArray(response);
        const kj::ArrayPtr<const kj::byte> bytes = words.asBytes();
        return {std::vector<std::uint8_t>(bytes.begin(), bytes.end()), {}};
    }

    Handler mHandler;
    TypedGate<RequestT> mGate;
    // Last: undeclaring joins the callback, which uses everything above.
    detail::ByteQueryable mQueryable;
};

}  // namespace pub_sub

#endif  // ZENOH_SERVICE_H_
