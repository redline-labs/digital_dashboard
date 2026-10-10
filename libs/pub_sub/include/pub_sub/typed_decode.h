#ifndef PUB_SUB_TYPED_DECODE_H_
#define PUB_SUB_TYPED_DECODE_H_

#include <capnp/serialize.h>

#include "pub_sub/capnp_payload.h"
#include "pub_sub/sample_check.h"
#include "pub_sub/schema_registry.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pub_sub
{

// The gate for a schema known as a type: its registry name and this build's
// fingerprint, both constants.
template <typename SchemaT>
struct TypedGate : SampleGate
{
    explicit TypedGate(std::string keyexpr) :
        SampleGate(std::move(keyexpr), schema_traits<SchemaT>::name, schema_traits<SchemaT>::layout)
    {
    }
};

// Hands `read` the payload as a SchemaT::Reader, valid for the call only.
// Returns false, having told the gate why, when the payload is not whole capnp
// words or capnp throws while it is read.
//
// The throw is caught around `read` and not just around the reader's
// construction: capnp reads lazily, so a damaged pointer surfaces when `read`
// follows it, not before. Call gate.admit() first -- this does not repeat it,
// so a caller that latches the name check is not charged for it per sample.
template <typename SchemaT, typename Read>
bool decodeAs(SampleGate& gate, const std::vector<std::uint8_t>& bytes, Read&& read)
{
    const WordAlignedPayload aligned(bytes);
    if (aligned.empty())
    {
        gate.refusePartialWord(bytes.size());
        return false;
    }
    try
    {
        capnp::FlatArrayMessageReader reader(aligned.words());
        std::forward<Read>(read)(reader.getRoot<SchemaT>());
        return true;
    }
    catch (const kj::Exception& e)
    {
        gate.refuseMalformed(e.getDescription().cStr());
        return false;
    }
}

}  // namespace pub_sub

#endif  // PUB_SUB_TYPED_DECODE_H_
