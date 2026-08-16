// SPDX-License-Identifier: GPL-3.0-or-later
//
// Matching a GNSS stream onto the road graph.
//
// ONLINE, not offline. OSRM's `match` takes a whole trace and finds the best
// path through it; this sees one fix at a time and has to answer immediately,
// which is a smaller problem: a short beam of candidates carried forward, each
// scored by how well it explains the new fix and how plausibly it follows from
// where we thought we were.
//
// THE SIGMA COMES FROM THE RECEIVER. A BD992 with an RTK fix reports
// centimetres; coasting on a lost correction link it reports metres. That is
// two orders of magnitude, and a matcher tuned for one silently jumps roads on
// the other -- so the emission width is taken from the fix rather than from a
// constant. This is the one thing this matcher has that an off-the-shelf one
// does not, and it is why the horizon publishes the sigma it used.
//
// Deliberately free of zenoh and capnp, exactly as bd992_bridge's epoch.h is,
// so the rule can be tested against a synthetic drive with no bus anywhere.
#ifndef MAP_MATCH_MATCHER_H
#define MAP_MATCH_MATCHER_H

#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

#include "road_graph/graph.h"
#include "road_graph/search.h"

namespace map_match
{

struct Fix
{
    road_graph::Coord lat { 0 };
    road_graph::Coord lon { 0 };

    // Course over ground. Absent when the receiver had no valid velocity, which
    // is the normal state of a stationary vehicle -- and a stationary vehicle
    // must not be re-matched on noise, so absence is meaningful rather than a
    // missing value to paper over.
    std::optional<double> headingDeg;
    double speedMps { 0.0 };

    // The receiver's own horizontal RMS, metres. Zero means it did not say.
    double sigmaM { 0.0 };
};

struct MatchResult
{
    bool matched { false };

    road_graph::SegmentIndex segment { road_graph::kNoSegment };
    std::uint32_t offsetCm { 0 };
    // Which way along the segment's own geometry we are travelling.
    bool forward { true };

    double headingDeg { 0.0 };
    // 0..100, how sure this is the right road -- NOT how accurate the fix is.
    std::uint8_t confidence { 0 };
    // The sigma actually used, so the answer can be explained after the fact.
    double sigmaUsedM { 0.0 };

    // True when the matcher moved to a different road this fix. Consumers that
    // cache anything keyed to the road want to know.
    bool changedRoad { false };
};

struct MatcherConfig
{
    // How far to look for candidates.
    double searchRadiusM { 50.0 };
    // How many to carry forward. Beyond a handful the extra candidates never
    // win, and each one costs a bounded Dijkstra per fix.
    std::size_t beamWidth { 6 };

    // Floor and ceiling on the emission width. The floor stops an RTK-fixed
    // receiver reporting 2 cm from making every candidate but one impossible --
    // OSM geometry is metres off, so believing the receiver completely would
    // match nothing at all. The ceiling stops a receiver that has lost its
    // corrections from making every road equally likely.
    double minSigmaM { 4.0 };
    double maxSigmaM { 30.0 };
    // Used when the receiver reports nothing.
    double defaultSigmaM { 8.0 };

    // How much a detour costs in the transition term. The difference between
    // the straight-line distance travelled and the on-graph distance, divided
    // by this, is the exponent -- so a road that requires a 2*beta detour to
    // explain the movement is e^-2 as likely.
    double transitionBetaM { 15.0 };

    // Below this, heading is noise: a stationary vehicle's course over ground
    // wanders through all 360 degrees and would re-match it every fix.
    double headingValidAboveMps { 1.5 };
};

class Matcher
{
  public:
    Matcher(const road_graph::Graph& graph, const MatcherConfig& config);

    MatchResult update(const Fix& fix);

    // Forget everything. After a gap in the stream, carrying the old beam
    // forward would explain a jump across town as a very long detour.
    void reset();

    // A SNAPSHOT, by value. update() runs on the zenoh RX thread and the node's
    // status timer reads these from the main loop, so the counters are atomics
    // and this hands back a copy -- returning a reference to them was a data
    // race, and the neighbouring fields in Services were already under a mutex
    // for exactly this reason.
    //
    // The four values are read independently, so a snapshot taken mid-update
    // may show `fixes` incremented before `matched`. That is fine for a status
    // topic and is not worth a lock on the matcher's hot path.
    struct Counts
    {
        std::uint64_t fixes { 0 };
        std::uint64_t matched { 0 };
        std::uint64_t unmatched { 0 };
        std::uint64_t resets { 0 };
    };

    Counts counts() const
    {
        return Counts { mFixes.load(std::memory_order_relaxed),
                        mMatched.load(std::memory_order_relaxed),
                        mUnmatched.load(std::memory_order_relaxed),
                        mResets.load(std::memory_order_relaxed) };
    }

  private:
    struct Candidate
    {
        road_graph::SegmentIndex segment { road_graph::kNoSegment };
        std::uint32_t offsetCm { 0 };
        bool forward { true };
        double bearingDeg { 0.0 };
        // Log-probability, accumulated. Log rather than probability because a
        // product of hundreds of small numbers underflows to zero within a
        // minute of driving, and then every candidate is equally impossible.
        double logProbability { 0.0 };
    };

    double sigmaFor(const Fix& fix) const;

    const road_graph::Graph& mGraph;
    MatcherConfig mConfig;

    // One reusable bounded-search context for the whole matcher. update()
    // makes up to beamWidth^2 of these per fix; a fresh hash table each time
    // was most of the cost of a fix. See road_graph::BoundedSearch.
    road_graph::BoundedSearch mSearch;

    std::vector<Candidate> mBeam;
    bool mHavePrevious { false };
    road_graph::Coord mPreviousLat { 0 };
    road_graph::Coord mPreviousLon { 0 };
    road_graph::SegmentIndex mPreviousSegment { road_graph::kNoSegment };

    std::atomic<std::uint64_t> mFixes { 0 };
    std::atomic<std::uint64_t> mMatched { 0 };
    std::atomic<std::uint64_t> mUnmatched { 0 };
    std::atomic<std::uint64_t> mResets { 0 };
};

} // namespace map_match

#endif // MAP_MATCH_MATCHER_H
