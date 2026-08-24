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
// THE MAPPING IS STRICTLY ONE TOPIC PER RECORD TYPE, and nothing else. Nothing
// here fuses, nothing here batches, nothing here decides which fields belong
// together, and nothing here knows or cares which records the receiver is
// configured to emit or how often. A record is decoded and put on its topic the
// moment it is parsed.
//
// That is what makes this node indifferent to the receiver's output
// configuration: enabling a message, disabling one, or moving one from 1 Hz to
// 50 Hz changes what appears on the bus and changes nothing here. The status
// message reports which record types have been seen and how long ago, which is
// where a configuration change becomes visible rather than silent.
//
// Deciding which records describe one instant belongs to the consumer, and past
// a single record that means a vehicle state estimator. See nodes/map_match for
// the shape of it: pair on arrival age, not on batch membership. Batch
// membership couples a consumer to the receiver's output schedule and stops
// being useful the moment two records run at different rates -- which is the
// normal case, not the exception.
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
