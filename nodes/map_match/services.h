// SPDX-License-Identifier: GPL-3.0-or-later
//
// The position subscription and the two publishers.
//
// THREADING: the position callback runs on a zenoh RX thread and the status
// timer on the main loop. The matcher's state is touched only from the
// callback, and everything the status reads is either behind mMutex or atomic
// -- see Matcher::counts(). Nothing here blocks: the receiver keeps sending
// while a callback runs, so the lock is never held across a match or a put.
#ifndef MAP_MATCH_SERVICES_H
#define MAP_MATCH_SERVICES_H

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>

#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"

#include "gsof_epoch.capnp.h"
#include "map_horizon.capnp.h"

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
    void onEpoch(const ::GsofEpoch::Reader& epoch);
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

    // Declared last, destroyed first, so a sample cannot arrive against a
    // handler whose state has already gone.
    std::optional<pub_sub::ZenohPublisher<::MapHorizon>> mHorizon;
    std::optional<pub_sub::ZenohPublisher<::MapMatchStatus>> mStatus;
    std::unique_ptr<pub_sub::ZenohTypedSubscriber<::GsofEpoch>> mPosition;
};

} // namespace map_match

#endif // MAP_MATCH_SERVICES_H
