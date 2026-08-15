// SPDX-License-Identifier: GPL-3.0-or-later
//
// One zenoh topic per GSOF record type.
//
// The mapping is entirely generated from GSOF_RECORD_TABLE: the schema for
// record `Name` is `Gsof##Name`, the topic is `<prefix>/gsof/<snake>`, and the
// publisher slot is a member of the same name. Adding a record type is a row
// in that table, a struct in libs/gsof, a capnp schema, and one fill()
// overload here -- and if you forget the fill(), it does not compile.
//
// PUBLISHERS ARE CREATED ON FIRST SIGHT of their record, not up front. The
// liveliness advertisements then name exactly what the receiver is actually
// sending: a topic that exists but has never published looks identical, in
// every picker in this tree, to one whose receiver has gone quiet.
//
// ONE topic is not a record: `<prefix>/epoch` carries the position, velocity,
// time and quality that arrived in the SAME transmission, fused. It is additive
// -- every per-record topic still publishes exactly as before -- and it exists
// because the record grouping is knowable here and nowhere downstream. See
// schemas/gsof_epoch.capnp.
//
// Everything here runs on the StreamClient's reader thread. ZenohPublisher is
// not thread-safe, and nothing else touches these.

#ifndef BD992_NODE_PUBLISHERS_H
#define BD992_NODE_PUBLISHERS_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "epoch.h"
#include "gsof/record_iterator.h"
#include "gsof/record_table.h"
#include "gsof/records.h"

namespace bd992_node
{

class Publishers
{
  public:
    Publishers(std::string topicPrefix, bool publishUnknownRecords);
    ~Publishers();

    Publishers(const Publishers&) = delete;
    Publishers& operator=(const Publishers&) = delete;

    // Decode and publish one record. Called on the reader thread.
    void publish(const gsof::RawRecord& raw);

    // End of a transmission: everything since the last call was sent together.
    // Publishes the fused GsofEpoch, if a position was among it, and clears
    // what was accumulated.
    //
    // This is the ONLY point in the system that knows which records belong to
    // one instant -- see schemas/gsof_epoch.capnp for why that matters and what
    // goes wrong downstream without it. Called on the reader thread, after the
    // publish() calls for that transmission.
    void endTransmission();

    // Fused epochs published, and how many were missing each component. A
    // component that is persistently absent means the receiver is not
    // configured to send those records together, which is a fault this node
    // cannot fix and must therefore report.
    struct EpochCounts
    {
        // How the transmissions were shaped, from the accumulator.
        EpochAccumulator::Counts shape {};

        // Of the epochs published, how many were missing each component.
        std::uint64_t withoutTime { 0 };
        std::uint64_t withoutVelocity { 0 };
        std::uint64_t withoutFixType { 0 };
        std::uint64_t withoutSigma { 0 };
    };

    EpochCounts epochCounts() const;

    // What has been seen, and how long ago. The status message's most useful
    // field: a receiver that quietly stopped sending one record is otherwise
    // indistinguishable from one that was never asked for it.
    struct Seen
    {
        std::uint8_t recordType { 0 };
        std::string recordName;
        std::uint64_t count { 0 };
        std::uint64_t ageMs { 0 };
    };

    std::vector<Seen> seen() const;

    // From GSOF 15, once it has been seen. Reported by the receiver-info
    // service, which would otherwise have nothing to say about identity
    // without a round trip to the control port.
    std::optional<std::int32_t> serialNumber() const;

    // Public because the generated record-to-publisher traits in the .cpp are
    // at file scope -- they are produced by expanding GSOF_RECORD_TABLE, which
    // cannot be done inside the class without also putting the fill()
    // overloads there. Defined only in the .cpp; nothing outside it can do
    // anything with the name.
    struct Impl;

  private:
    std::unique_ptr<Impl> mImpl;
};

} // namespace bd992_node

#endif // BD992_NODE_PUBLISHERS_H
