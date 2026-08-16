// SPDX-License-Identifier: GPL-3.0-or-later
#include "matcher.h"

#include <algorithm>
#include <cmath>

#include "road_graph/search.h"

namespace map_match
{
namespace
{

// Log of a Gaussian emission, without the constant term -- only differences
// between candidates matter, so the normalisation would cancel.
double logEmission(double distanceM, double sigmaM)
{
    const double z = distanceM / sigmaM;
    return -0.5 * z * z;
}

} // namespace

Matcher::Matcher(const road_graph::Graph& graph, const MatcherConfig& config) :
    mGraph(graph),
    mConfig(config)
{
}

void Matcher::reset()
{
    mBeam.clear();
    mHavePrevious = false;
    mPreviousSegment = road_graph::kNoSegment;
    mResets.fetch_add(1, std::memory_order_relaxed);
}

double Matcher::sigmaFor(const Fix& fix) const
{
    // THE point of this matcher. An RTK-fixed receiver reports centimetres and
    // a coasting one reports metres; using a constant for both means one of
    // them is wrong by two orders of magnitude, and the failure is silent --
    // too tight and nothing matches, too loose and everything does.
    //
    // Clamped at both ends, and the floor is the interesting one: OSM
    // centrelines are metres from the real road, so believing a 2 cm fix
    // completely would make every candidate impossible. The floor is really a
    // statement about the MAP's accuracy, not the receiver's.
    const double reported = fix.sigmaM > 0.0 ? fix.sigmaM : mConfig.defaultSigmaM;
    return std::clamp(reported, mConfig.minSigmaM, mConfig.maxSigmaM);
}

MatchResult Matcher::update(const Fix& fix)
{
    mFixes.fetch_add(1, std::memory_order_relaxed);

    MatchResult result;
    const double sigma = sigmaFor(fix);
    result.sigmaUsedM = sigma;

    // Heading is only used when the vehicle is actually moving. A stationary
    // vehicle's course over ground wanders through all 360 degrees, and feeding
    // that in re-matches it onto a different road every fix while it sits at a
    // light.
    std::optional<double> heading;
    if (fix.headingDeg.has_value() && fix.speedMps >= mConfig.headingValidAboveMps)
    {
        heading = fix.headingDeg;
    }

    // Search wider when the fix is poor: a radius that made sense at 2 cm finds
    // nothing at 20 m.
    const double radius = std::max(mConfig.searchRadiusM, sigma * 3.0);
    const auto candidates = mGraph.nearest(fix.lat, fix.lon, radius, mConfig.beamWidth, heading);

    if (candidates.empty())
    {
        // NORMAL: a car park, a private drive, a road that is not in the map.
        // The beam is dropped rather than carried, because the next fix has no
        // trustworthy predecessor to have come from.
        mUnmatched.fetch_add(1, std::memory_order_relaxed);
        mBeam.clear();
        mHavePrevious = true;
        mPreviousLat = fix.lat;
        mPreviousLon = fix.lon;
        return result;
    }

    // How far the vehicle actually moved since the last fix. The transition term
    // asks how well each candidate's on-graph distance explains that.
    double travelledM = 0.0;
    if (mHavePrevious)
    {
        travelledM = road_graph::distanceM(mPreviousLat, mPreviousLon, fix.lat, fix.lon);
    }

    std::vector<Candidate> next;
    next.reserve(candidates.size());

    for (const road_graph::Match& match : candidates)
    {
        Candidate candidate;
        candidate.segment = match.segment;
        candidate.offsetCm = match.offsetCm;
        candidate.bearingDeg = match.bearingDeg;
        candidate.forward =
            !heading.has_value() ||
            road_graph::bearingDeltaDeg(*heading, match.bearingDeg) <= 90.0;

        candidate.logProbability = logEmission(match.distanceM, sigma);

        if (!mBeam.empty())
        {
            // Best transition from any previous candidate. The alternative --
            // keeping every (previous, current) pair -- is the full Viterbi
            // lattice, which for an online matcher that only ever reports the
            // current best is more bookkeeping than answer.
            double bestTransition = -1e9;
            for (const Candidate& previous : mBeam)
            {
                double onGraphM = 0.0;
                if (previous.segment == candidate.segment)
                {
                    // Same road: the distance is just how far along it we moved,
                    // and no search is needed. This is the overwhelmingly common
                    // case at 10 Hz, and short-circuiting it is what keeps the
                    // matcher cheap.
                    onGraphM = std::fabs(static_cast<double>(candidate.offsetCm) -
                                         static_cast<double>(previous.offsetCm)) /
                               100.0;
                }
                else
                {
                    const road_graph::SegmentRecord& from = mGraph.segments()[previous.segment];
                    const road_graph::SegmentRecord& to = mGraph.segments()[candidate.segment];

                    // Bounded hard. Two candidates that do not connect within a
                    // few times the distance travelled are not a plausible pair,
                    // and searching further to prove it would expand the city.
                    const double limit = std::max(200.0, travelledM * 4.0);
                    const auto distance = mSearch.distance(
                        mGraph, previous.forward ? from.toNode : from.fromNode,
                        candidate.forward ? to.fromNode : to.toNode, limit);
                    if (!distance)
                    {
                        continue;
                    }
                    onGraphM = *distance;
                }

                const double detour = std::fabs(onGraphM - travelledM);
                const double transition = -detour / mConfig.transitionBetaM;
                bestTransition = std::max(bestTransition, previous.logProbability + transition);
            }

            if (bestTransition < -1e8)
            {
                // Nothing in the beam can reach this candidate. Not impossible
                // -- the beam may be stale -- so it keeps its emission score and
                // is penalised rather than dropped.
                candidate.logProbability -= 10.0;
            }
            else
            {
                candidate.logProbability += bestTransition;
            }
        }

        next.push_back(candidate);
    }

    // Renormalise so the best candidate sits at zero. Without this the scores
    // march towards negative infinity over a drive and eventually stop being
    // representable; with it the beam only ever carries differences, which is
    // all it uses.
    const auto best = std::max_element(next.begin(), next.end(),
                                       [](const Candidate& a, const Candidate& b) {
                                           return a.logProbability < b.logProbability;
                                       });
    const double top = best->logProbability;
    for (Candidate& candidate : next)
    {
        candidate.logProbability -= top;
    }

    std::sort(next.begin(), next.end(), [](const Candidate& a, const Candidate& b) {
        return a.logProbability > b.logProbability;
    });
    if (next.size() > mConfig.beamWidth)
    {
        next.resize(mConfig.beamWidth);
    }

    const Candidate& winner = next.front();

    // Confidence is how far clear the winner is of the runner-up, not how close
    // the fix was to the road. A fix sitting exactly between two parallel roads
    // is a confident POSITION and an unconfident MATCH, and conflating them is
    // how a display ends up looking certain about the wrong road.
    //
    // After renormalisation the winner sits at zero and everything else is
    // negative, so the runner-up's score IS the margin.
    double margin = 4.0;
    if (next.size() > 1)
    {
        margin = -next[1].logProbability;
    }
    result.confidence = static_cast<std::uint8_t>(std::clamp(margin / 4.0, 0.0, 1.0) * 100.0);

    result.matched = true;
    result.segment = winner.segment;
    result.offsetCm = winner.offsetCm;
    result.forward = winner.forward;
    result.headingDeg = winner.forward ? winner.bearingDeg
                                       : std::fmod(winner.bearingDeg + 180.0, 360.0);
    result.changedRoad = mPreviousSegment != road_graph::kNoSegment &&
                         mPreviousSegment != winner.segment;

    mBeam = std::move(next);
    mHavePrevious = true;
    mPreviousLat = fix.lat;
    mPreviousLon = fix.lon;
    mPreviousSegment = winner.segment;
    mMatched.fetch_add(1, std::memory_order_relaxed);

    return result;
}

} // namespace map_match
