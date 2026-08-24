// SPDX-License-Identifier: GPL-3.0-or-later
#include "services.h"

#include <random>

#include <spdlog/spdlog.h>

#include "horizon.h"
#include "map_rules/classification.h"
#include "map_wire/segment.h"

namespace map_match
{
namespace
{

MatcherConfig matcherConfigOf(const MatchConfig& config)
{
    MatcherConfig out;
    out.searchRadiusM = config.searchRadiusM;
    out.beamWidth = config.beamWidth;
    out.minSigmaM = config.minSigmaM;
    out.maxSigmaM = config.maxSigmaM;
    out.defaultSigmaM = config.defaultSigmaM;
    out.transitionBetaM = config.transitionBetaM;
    out.headingValidAboveMps = config.headingValidAboveMps;
    return out;
}

} // namespace

Services::Services(const NodeConfig& config, const road_graph::Graph& graph) :
    mConfig(config),
    mGraph(graph),
    mMatcher(graph, matcherConfigOf(config.match)),
    mAssembler(std::chrono::milliseconds(config.position.pairWithinMs))
{
    std::random_device device;
    mSessionNonce = (static_cast<std::uint64_t>(device()) << 32) | device();

    mHorizon.emplace(config.services.horizonKey);
    mStatus.emplace(config.services.statusKey);

    // The position is the trigger: it is the record the matcher runs on. The
    // other two are held as latest-known and paired by age -- see
    // fix_assembler.h.
    mPosition = std::make_unique<pub_sub::ZenohTypedSubscriber<::GsofLatLongHeight>>(
        config.position.positionKey, [this](::GsofLatLongHeight::Reader record) {
            const double lat = record.getLatitudeDeg();
            const double lon = record.getLongitudeDeg();
            AssembledFix assembled;
            {
                const std::lock_guard<std::mutex> lock(mAssemblerMutex);
                assembled = mAssembler.onPosition(lat, lon, std::chrono::steady_clock::now());
            }
            // Outside the lock: the match is the expensive part, and the record
            // callbacks must not queue behind each other on it.
            onFix(assembled);
        });

    mVelocity = std::make_unique<pub_sub::ZenohTypedSubscriber<::GsofVelocity>>(
        config.position.velocityKey, [this](::GsofVelocity::Reader record) {
            VelocitySample sample;
            sample.valid = record.getValid();
            sample.headingDeg = record.getHeadingDeg();
            sample.speedMps = record.getHorizontalSpeedMps();
            const std::lock_guard<std::mutex> lock(mAssemblerMutex);
            mAssembler.setVelocity(sample, std::chrono::steady_clock::now());
        });

    mSigma = std::make_unique<pub_sub::ZenohTypedSubscriber<::GsofPositionSigma>>(
        config.position.sigmaKey, [this](::GsofPositionSigma::Reader record) {
            const float rms = record.getPositionRms();
            const std::lock_guard<std::mutex> lock(mAssemblerMutex);
            mAssembler.setSigma(rms, std::chrono::steady_clock::now());
        });

    SPDLOG_INFO("[node] matching '{}' (+ '{}', '{}' within {} ms) -> '{}'",
                config.position.positionKey, config.position.velocityKey, config.position.sigmaKey,
                config.position.pairWithinMs, config.services.horizonKey);
}

void Services::onFix(const AssembledFix& assembled)
{
    // ON A ZENOH RX THREAD, and not necessarily the same one each time. Must
    // not block: the receiver keeps sending.
    const auto now = std::chrono::steady_clock::now();

    // Under the lock because publishStatus() reads both from the main loop.
    // Held only across the two loads and the two stores -- never across the
    // match itself.
    bool haveFix = false;
    std::chrono::steady_clock::time_point lastFix {};
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        haveFix = mHaveFix;
        lastFix = mLastFix;
        mLastFix = now;
        mHaveFix = true;
    }

    if (haveFix)
    {
        const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFix);
        if (gap.count() > mConfig.position.staleAfterMs)
        {
            // A gap in the stream. Carrying the beam across it would explain a
            // jump across town as a very long detour, and the matcher would
            // then be confidently wrong for as long as it took to recover.
            mMatcher.reset();
        }
    }

    Fix fix;
    fix.lat = road_graph::fromDegrees(assembled.latitudeDeg);
    fix.lon = road_graph::fromDegrees(assembled.longitudeDeg);

    // Absent rather than stale. A heading old enough to be from before a turn
    // is worse than no heading at all: the matcher handles absence by falling
    // back on distance, and handles a wrong heading by matching confidently
    // onto the wrong road.
    if (assembled.hasVelocity && assembled.velocityValid)
    {
        fix.headingDeg = assembled.headingDeg;
        fix.speedMps = assembled.speedMps;
    }
    if (assembled.hasSigma)
    {
        // The receiver's own RMS. This is what makes the matcher behave
        // differently on an RTK fix and on a coasting one -- see matcher.h.
        fix.sigmaM = assembled.positionRmsM;
    }

    const MatchResult match = mMatcher.update(fix);

    {
        const std::lock_guard<std::mutex> lock(mMutex);
        ++mFixesReceived;
        mLastConfidence = match.confidence;
        mLastSigmaM = static_cast<float>(match.sigmaUsedM);
    }

    // Rate-limited here rather than in the matcher: the matcher runs at whatever
    // the receiver sends, and the bus sees a fixed rate.
    //
    // mLastPublish is touched only here, and onFix() is reached only from the
    // position subscription -- one zenoh RX thread per subscription, so these
    // calls never overlap.
    const auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - mLastPublish);
    if (since.count() < mConfig.services.horizonIntervalMs)
    {
        return;
    }
    mLastPublish = now;

    publishHorizon(fix, match);
}

void Services::publishHorizon(const Fix& fix, const MatchResult& match)
{
    ::MapHorizon::Builder out = mHorizon->fields();

    out.setSequence(mSequence++);
    out.setSessionNonce(mSessionNonce);

    if (match.changedRoad)
    {
        // The path tree has been re-anchored, so offsets from before this mean
        // nothing. A consumer caching "sharp curve at 120 m" must discard it.
        ++mEpoch;
    }
    out.setHorizonEpoch(mEpoch);

    out.setHasPosition(match.matched);
    if (!match.matched)
    {
        // Published anyway. A subscriber has to be able to tell "the matcher is
        // running and lost" from "the matcher is dead", and an absent message
        // says the second when it means the first.
        out.initPaths(0);
        out.initProfiles(0);
        mHorizon->put();
        return;
    }

    const Horizon horizon = buildHorizon(mGraph, match.segment, match.offsetCm, match.forward,
                                         mConfig.match.lookaheadM * 100);

    auto position = out.initPosition();
    position.setPathId(1);
    position.setOffsetCm(horizon.positionOffsetCm);
    position.setHeadingDeg(static_cast<float>(match.headingDeg));
    position.setConfidence(match.confidence);
    position.setSigmaM(static_cast<float>(match.sigmaUsedM));
    // The RAW fix, unmodified. The horizon says which road we are on; it does
    // not move the vehicle. With an RTK fix the receiver is more accurate than
    // OSM geometry, so snapping would make the displayed position worse.
    position.setLatitudeDeg(road_graph::toDegrees(fix.lat));
    position.setLongitudeDeg(road_graph::toDegrees(fix.lon));

    auto where = position.initWhere();
    where.setSegmentId(mGraph.segments()[match.segment].id);
    where.setOffsetCm(match.offsetCm);
    where.setForward(match.forward);

    // One path: the road we are on and its unambiguous continuation. Branches
    // are extra entries here, and nothing else changes when they arrive.
    auto paths = out.initPaths(1);
    paths[0].setPathId(1);
    paths[0].setParentPathId(0);
    paths[0].setOffsetOnParentCm(0);
    paths[0].setLengthCm(horizon.lengthCm);
    paths[0].setProbability(100);

    // Five profiles per run: name, ref, class, speed, segment. Runs tile the
    // path with no gaps by construction, and each value type expresses its own
    // absence -- an empty name, MapSpeed.hasPosted false -- rather than leaving
    // a hole a consumer would interpolate across.
    const unsigned perRun = 5;
    auto profiles = out.initProfiles(static_cast<unsigned>(horizon.runs.size()) * perRun);

    unsigned at = 0;
    for (const HorizonRun& run : horizon.runs)
    {
        const road_graph::SegmentRecord& segment = mGraph.segments()[run.segment];

        // The union discriminant IS the profile type -- there is deliberately
        // no separate `type` field beside it, because two fields saying what a
        // profile is could disagree, silently. So every setter below goes
        // through getValue().
        const auto fill = [&](unsigned index) {
            auto profile = profiles[index];
            profile.setPathId(1);
            profile.setStartOffsetCm(run.startOffsetCm);
            profile.setEndOffsetCm(run.endOffsetCm);
            return profile.getValue();
        };

        fill(at++).setRoadName(std::string(mGraph.nameOf(segment)));
        fill(at++).setRoadRef(std::string(mGraph.refOf(segment)));
        fill(at++).setRoadClass(map_wire::classOf(segment.routeClass));

        map_wire::fillSpeed(fill(at++).initSpeed(), segment);

        fill(at++).setSegment(run.segmentId);
    }

    mHorizon->put();

    const std::lock_guard<std::mutex> lock(mMutex);
    ++mHorizonsPublished;
}

void Services::publishStatus()
{
    ::MapMatchStatus::Builder out = mStatus->fields();

    out.setGraph("");
    out.setGraphOpen(true);
    out.setError("");
    out.setPositionKey(mConfig.position.positionKey);
    out.setVelocityKey(mConfig.position.velocityKey);
    out.setSigmaKey(mConfig.position.sigmaKey);

    // How often a position found no fresh velocity or accuracy to pair with.
    // THIS IS WHERE A RECEIVER RECONFIGURATION BECOMES VISIBLE: move velocity
    // to a slower rate than position, or turn it off, and these climb instead
    // of the matcher quietly running on distance alone.
    FixAssembler::Counts pairing;
    {
        const std::lock_guard<std::mutex> lock(mAssemblerMutex);
        pairing = mAssembler.counts();
    }
    out.setFixesWithoutVelocity(pairing.withoutVelocity);
    out.setFixesWithoutSigma(pairing.withoutSigma);

    const Matcher::Counts counts = mMatcher.counts();
    out.setFixesMatched(counts.matched);
    out.setFixesUnmatched(counts.unmatched);

    // Grows without bound when the receiver stops, which is the signal that
    // this node is fine and its input is not.
    std::int64_t age = 0;
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        out.setFixesReceived(mFixesReceived);
        out.setHorizonsPublished(mHorizonsPublished);
        out.setLastConfidence(mLastConfidence);
        out.setLastSigmaM(mLastSigmaM);

        // Read here rather than after the lock: mLastFix is written on the RX
        // thread on every fix.
        age = mHaveFix ? std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - mLastFix)
                             .count()
                       : 0;
    }
    out.setLastFixAgeMs(static_cast<std::uint64_t>(age));

    mStatus->put();
}

} // namespace map_match
