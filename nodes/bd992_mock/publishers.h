// SPDX-License-Identifier: GPL-3.0-or-later
//
// Putting a simulated vehicle onto the BD992 topics.
//
// THIS IS A SECOND IMPLEMENTATION OF A CONTRACT nodes/bd992_bridge already
// implements, and that is a deliberate, accepted cost: this node never sees a
// GSOF byte, so there is nothing to hand the bridge's decoder, and reaching into
// the bridge to share its publishers would couple a mock to the shape of the
// real node's internals. The price is that the two can drift -- a field this
// forgets to set, or sets differently, is silent, because capnp reads a
// half-filled message as zeroes rather than as an error. docs/bd992.md's
// key/schema table is the reference, and `inspect echo` against both is how a
// divergence gets noticed.
//
// FIVE TOPICS, ONE PER RECORD TYPE, matching the bridge exactly.
//
// All five go out together on each tick, which is what a receiver does when
// those records share an output rate. It is therefore NOT a test of a consumer
// that has to cope with records arriving at different rates -- on real hardware
// the accuracy and quality records commonly run slower than position. A
// consumer that pairs records by arrival age (see nodes/map_match) will see
// every pairing succeed here and should not be assumed correct on that basis.
//
// Everything here runs on the main thread. ZenohPublisher is not thread-safe
// and nothing else touches these.

#ifndef BD992_MOCK_PUBLISHERS_H
#define BD992_MOCK_PUBLISHERS_H

#include <cstdint>
#include <memory>
#include <random>

#include "node_config.h"
#include "vehicle.h"

namespace bd992_mock
{

// GPS time, as the two fields every GSOF record carries it in.
struct GpsTime
{
    std::uint16_t week { 0 };
    std::uint32_t timeOfWeekMs { 0 };
};

// Unix seconds (UTC) to GPS week and time of week.
//
// GPS time does not observe leap seconds, so it runs AHEAD of UTC by however
// many have been inserted since 1980 -- 18 as of this writing. Ignoring that
// offset would put every timestamp 18 seconds in the past, which looks entirely
// reasonable and is wrong. A real receiver reports the offset in record 16; a
// mock has to be told, hence the constant.
GpsTime gpsTimeFromUnix(double unixSeconds);

class Publishers
{
  public:
    explicit Publishers(const PublishConfig& config);
    ~Publishers();

    Publishers(const Publishers&) = delete;
    Publishers& operator=(const Publishers&) = delete;

    // One tick: all five record topics, published back to back.
    void publish(const VehicleState& state, const GpsTime& time);

    std::uint32_t sequence() const { return mSequence; }

  private:
    struct Impl;

    PublishConfig mConfig;
    std::unique_ptr<Impl> mImpl;

    std::uint32_t mSequence { 0 };
    std::mt19937 mNoise;
};

} // namespace bd992_mock

#endif // BD992_MOCK_PUBLISHERS_H
