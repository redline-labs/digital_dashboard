// SPDX-License-Identifier: GPL-3.0-or-later
//
// MTData2 items onto zenoh topics.
//
// EVERYTHING HERE RUNS ON THE StreamClient'S READER THREAD. ZenohPublisher is
// not thread-safe, and nothing else touches these. The status publisher in
// main.cpp is a separate one on the main thread, deliberately.
//
// The model is nodes/bd992_bridge/publishers.h's, with one deliberate
// difference:
//
//   * One topic per data identifier, and publishers are created ON FIRST SIGHT
//     rather than up front, so a topic's liveliness advertisement names
//     something the device is really sending.
//
//   * Nothing here fuses and nothing here batches. Which measurements belong
//     to one instant is not this node's decision.
//
//   * BUT the packet header rides on every message. That is the difference. A
//     GSOF record carries its own gpsTimeMs; an MTData2 item does not -- the
//     counter, the sample time and the status word are properties of the
//     PACKET. Publishing them on their own topics would leave a consumer
//     pairing by arrival order, which is the guess libs/map_match has to make
//     for GNSS and does not have to make here. See schemas/xbus_common.capnp.
//
// Unmodelled items go out as raw bytes rather than being dropped, because a
// device emitting something unrecognised is otherwise indistinguishable from
// one that is silent -- and on an MTi-610 a busy raw topic means the device is
// not a 610.

#ifndef MTI610_PUBLISHERS_H
#define MTI610_PUBLISHERS_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xbus/message.h"

namespace mti610_node
{

// What the status message reports about which items have been seen.
struct SeenItem
{
    std::uint16_t rawDataId { 0 };
    std::string name;
    std::uint64_t count { 0 };
};

class Publishers
{
  public:
    Publishers(std::string topicPrefix, bool publishUnknownItems);
    ~Publishers();

    Publishers(const Publishers&) = delete;
    Publishers& operator=(const Publishers&) = delete;
    Publishers(Publishers&&) = delete;
    Publishers& operator=(Publishers&&) = delete;

    // Walk one MTData2 and publish what is in it. Called on the reader thread.
    void publish(const xbus::MessageView& message);

    // For the status message. Safe from another thread.
    std::vector<SeenItem> seen() const;

    // Public only so the publisher-slot traits in publishers.cpp can name it.
    // It is an opaque forward declaration, so nothing about the class is
    // exposed by this.
    struct Impl;

  private:
    std::unique_ptr<Impl> mImpl;
};

} // namespace mti610_node

#endif // MTI610_PUBLISHERS_H
