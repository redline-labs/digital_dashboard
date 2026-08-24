// SPDX-License-Identifier: GPL-3.0-or-later
//
// The three record subscriptions, the pairing that assembles a fix from them,
// and the two publishers.
//
// WHY THREE SUBSCRIPTIONS. The bridge publishes one topic per GSOF record type
// and nothing else; deciding which records describe one instant is a consumer's
// job. This consumer needs position, heading and accuracy -- records 2, 8 and
// 12 -- and none of them carries a time.
//
// THE POSITION IS THE TRIGGER. A fix is assembled and matched when a position
// arrives; velocity and accuracy are held as latest-known and used only if they
// are fresh enough to describe the same moment. See fix_assembler.h for why
// that beats grouping by GSOF transmission, and why it is the shape that
// survives the receiver being reconfigured to run position at 50 Hz and status
// at 1 Hz.
//
// THREADING: the three record callbacks run on zenoh RX threads and may run
// CONCURRENTLY. mAssembler is behind mAssemblerMutex, which is held across the
// held-value updates and across assembling a fix, but never across the match
// itself. The status timer runs on the main loop and reads mMutex. Nothing here
// blocks: the receiver keeps sending while a callback runs.

#ifndef MAP_MATCH_SERVICES_H
#define MAP_MATCH_SERVICES_H

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>

#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"

#include "gsof_position.capnp.h"
#include "map_horizon.capnp.h"

#include "fix_assembler.h"
#include "matcher.h"
#include "node_config.h"

namespace map_match
{

class Services
{
  public:
    Services(const NodeConfig& config, const road_graph::Graph& graph);

    // Called from the main loop on a timer, never from the RX thread.
    void publishStatus();

  private:
    void onFix(const AssembledFix& assembled);
    void publishHorizon(const Fix& fix, const MatchResult& match);

    const NodeConfig& mConfig;
    const road_graph::Graph& mGraph;
    Matcher mMatcher;

    // Randomised at start. Without it a restart resets `sequence` to zero and a
    // subscriber reads the jump backwards as reordering rather than as a new
    // session, and quietly keeps stale state.
    std::uint64_t mSessionNonce { 0 };
    std::uint32_t mSequence { 0 };
    std::uint32_t mEpoch { 0 };

    // RX thread only: publishHorizon() is called from onEpoch() and nowhere
    // else, so these need no guard.
    std::chrono::steady_clock::time_point mLastPublish {};

    mutable std::mutex mMutex;
    // Written on the RX thread, read by publishStatus() on the main loop.
    std::chrono::steady_clock::time_point mLastFix {};
    bool mHaveFix { false };
    std::uint64_t mFixesReceived { 0 };
    std::uint64_t mHorizonsPublished { 0 };
    std::uint8_t mLastConfidence { 0 };
    float mLastSigmaM { 0.0F };

    // Guards mAssembler only, and is taken by all three record callbacks.
    // Separate from mMutex so the status timer never contends with the record
    // stream. Never held across a match.
    std::mutex mAssemblerMutex;
    FixAssembler mAssembler;

    // Declared last, destroyed first, so a sample cannot arrive against a
    // handler whose state has already gone.
    std::optional<pub_sub::ZenohPublisher<::MapHorizon>> mHorizon;
    std::optional<pub_sub::ZenohPublisher<::MapMatchStatus>> mStatus;
    std::unique_ptr<pub_sub::ZenohTypedSubscriber<::GsofLatLongHeight>> mPosition;
    std::unique_ptr<pub_sub::ZenohTypedSubscriber<::GsofVelocity>> mVelocity;
    std::unique_ptr<pub_sub::ZenohTypedSubscriber<::GsofPositionSigma>> mSigma;
};

} // namespace map_match

#endif // MAP_MATCH_SERVICES_H
