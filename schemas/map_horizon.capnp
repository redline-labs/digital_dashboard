@0x9e6ac79e2c6d6c4e;

using Common = import "map_common.capnp";

# The electronic horizon: where we are on the road network, and what is ahead.
#
# Published by nodes/map_match, which matches the GNSS stream onto the road
# graph. Consumers are a road-name display today and, later, anything that
# wants to know about the road before reaching it -- powertrain, dampers,
# a curve-speed warning.
#
# The data model is ADASIS's, because ADASIS solved this shape: a TREE OF PATHS
# rather than one road, positions as (path, offset) rather than coordinates,
# and attributes as runs along a path rather than per-point samples.
#
# The TRANSPORT is deliberately NOT ADASIS's. ADASIS is incremental with cyclic
# counters because it was designed to fit 8-byte CAN frames; a dropped delta
# there silently repositions every later attribute, forever. This is zenoh and
# capnp, a 2 km horizon is a few kB, and so every message is a COMPLETE
# SNAPSHOT. If that ever has to change, it changes by adding a message class,
# not by reinterpreting this one.
#
# WHAT THIS DOES NOT DO: correct the position. It says which segment the
# vehicle is on; dashboard/widgets/map keeps drawing the receiver's own fix.
# The position source is a BD992 with an RTK fix, which is accurate to
# centimetres, while OSM road geometry is routinely several metres from the
# real centreline -- so snapping the displayed position to the map would make
# it WORSE. This is the obvious-looking "improvement" that must not be made.

# ============================================================================
# Paths
# ============================================================================

# One path through the network, as a flat entry with a parent link.
#
# NOT a nested `List(HorizonPath)`. libs/pub_sub builds every
# capnp::FlatArrayMessageReader with default ReaderOptions, whose nestingLimit
# is 64 -- past that capnp throws and this tree drops the sample with nothing
# to show for it. Flat is also what ADASIS does, and it makes a consumer's
# index a plain array lookup.
struct HorizonPath {
  # Unique within one message only. Path ids are NOT stable across messages;
  # see MapHorizon.horizonEpoch.
  pathId @0 :UInt32;

  # Zero on the root path -- the one the vehicle is on. Every other path
  # branches from a parent at `offsetOnParentCm` along it.
  parentPathId @1 :UInt32;
  offsetOnParentCm @2 :UInt32;

  # How far this path has been built out. A consumer must not extrapolate
  # past it.
  lengthCm @3 :UInt32;

  # 0..100. How likely the vehicle is to take this path, given no route is
  # active. The root path is not automatically 100: at a fork, two children can
  # each be 50 and the parent simply ends.
  probability @4 :UInt8;
}

# ============================================================================
# Profiles
# ============================================================================

# One attribute, constant over a stretch of one path.
#
# THE UNION DISCRIMINANT IS THE PROFILE TYPE. There is deliberately no separate
# `type` enum beside the value: two fields saying what this is could disagree,
# and the disagreement would be silent.
#
# CONSUMERS MUST FILTER, NOT SWITCH. Take the profile kinds you understand and
# ignore the rest. This is a deliberate carve-out from this tree's usual
# "spell out every case in a switch" rule (AGENTS.md), and it is the entire
# reason the horizon can grow: adding `curvature` later must not break every
# node that already subscribes. capnp hands an unknown discriminant to an old
# reader as a raw number outside its generated enum, so a switch over
# `which()` would fall through every case -- filtering is not a style
# preference here, it is the only correct shape.
#
# RUNS OF ONE KIND MUST TILE THEIR PATH WITH NO GAPS. Where an attribute is not
# known, emit a run that SAYS so in its own vocabulary -- MapSpeed.hasPosted
# false, MapRoadClass.unknown, empty Text -- rather than leaving a hole. A hole
# is indistinguishable from "not computed yet", and a consumer that interpolates
# across one shows a 25 mph limit for a mile of freeway.
struct HorizonProfile {
  pathId @0 :UInt32;

  startOffsetCm @1 :UInt32;
  # MANDATORY, and never open-ended. A run without an end is one a consumer
  # extends to the horizon, which is how a school-zone limit ends up displayed
  # on an interstate.
  endOffsetCm @2 :UInt32;

  value :union {
    # Never sent. Present so a default-constructed profile is not silently a
    # meaningful one.
    unknown @3 :Void;

    roadName @4 :Text;
    roadRef @5 :Text;
    roadClass @6 :Common.MapRoadClass;
    speed @7 :Common.MapSpeed;
    laneCount @8 :UInt8;

    # Which segment of the graph this stretch of path came from. This is the
    # join back to map_graph.capnp, to the router, and eventually to the
    # feature drawn in a vector tile.
    segment @9 :UInt64;

    # Room to grow, in the order they are likely to arrive: curvature
    # (1/m, signed, left positive), slopePercent, surface, tunnel, bridge.
    # Each is a new union member and breaks nothing.
  }
}

# ============================================================================
# Where we are
# ============================================================================

struct HorizonPosition {
  # Always on the root path, by construction.
  pathId @0 :UInt32;
  offsetCm @1 :UInt32;

  # The graph segment, and which way along it.
  where @2 :Common.MapSegmentRef;

  # Direction of travel, degrees clockwise from true north.
  headingDeg @3 :Float32;

  # 0..100. How sure the matcher is that `where` is the right road, not how
  # accurate the fix is. Low confidence beside a frontage road is the normal
  # case, not a fault.
  confidence @4 :UInt8;

  # The position uncertainty the matcher ACTUALLY USED for this fix, in metres,
  # taken from the receiver rather than hardcoded. An RTK-fixed BD992 reports
  # centimetres and an autonomous fix reports metres, and a matcher tuned for
  # one silently jumps roads on the other -- so this is published, because it is
  # the first thing to look at when a match is wrong and it makes the failure
  # visible in `scope` with no special tool.
  sigmaM @5 :Float32;

  # The fix as received, unmodified. Present so a consumer can see how far the
  # match moved the answer -- and so nothing downstream has to go and find the
  # receiver's own topic to draw the vehicle in the right place.
  latitudeDeg @6 :Float64;
  longitudeDeg @7 :Float64;
}

# ============================================================================
# The message
# ============================================================================

struct MapHorizon {
  # Monotonic within one publisher session, incrementing by one per message.
  # The bus provides no sequence number and no delivery guarantee, so this is
  # the only way a subscriber can notice it missed one.
  sequence @0 :UInt32;

  # Randomised when nodes/map_match starts. Without it, a restart resets
  # `sequence` to zero and a subscriber reads the jump as reordering rather
  # than as a new session -- and quietly keeps stale state.
  sessionNonce @1 :UInt64;

  # Bumped whenever the path tree is re-anchored, which happens on a re-match,
  # a branch resolving, or the vehicle passing a junction. PATH IDS AND OFFSETS
  # MEAN NOTHING ACROSS AN EPOCH CHANGE: a consumer caching "sharp curve at
  # 120 m" must discard it when this number moves. Within one epoch, offsets on
  # a given path are comparable between messages.
  horizonEpoch @2 :UInt32;

  # Absent (`hasPosition` false) when the matcher has no fix or no match. The
  # message is still published, so a subscriber can tell "matcher is running
  # and lost" from "matcher is dead".
  hasPosition @3 :Bool;
  position @4 :HorizonPosition;

  # The root path is first. Bounded by the publisher -- see
  # nodes/map_match's config -- so a matcher bug cannot put a 100 MB message
  # on the bus.
  paths @5 :List(HorizonPath);
  profiles @6 :List(HorizonProfile);
}

# ============================================================================
# Node status
# ============================================================================

# What nodes/map_match is doing, published on a timer.
#
# This exists so a blank road name can be told apart from a dead node, the same
# argument as MapServerStatus: both look identical in a screenshot.
struct MapMatchStatus {
  graph @0 :Text;
  graphOpen @1 :Bool;
  # Empty unless the graph failed to open.
  error @2 :Text;

  # The topics being consumed, so a wrong key is visible without reading the
  # YAML. Three of them, joined on the GSOF transmission number: the bridge
  # publishes one topic per record type and none of the three carries a time,
  # so the join is the only thing that keeps a heading with its own position.
  positionKey @3 :Text;

  fixesReceived @4 :UInt64;
  fixesMatched @5 :UInt64;
  # Fixes with no candidate within the search radius. Expected to be non-zero:
  # car parks and private roads are not in the graph.
  fixesUnmatched @6 :UInt64;
  horizonsPublished @7 :UInt64;

  # Milliseconds since the last fix arrived. Grows without bound when the
  # receiver stops, which is the signal that this node is fine and its input
  # is not.
  lastFixAgeMs @8 :UInt64;

  lastConfidence @9 :UInt8;
  lastSigmaM @10 :Float32;

  velocityKey @11 :Text;
  sigmaKey @12 :Text;

  # Positions that found no velocity, or no accuracy, recent enough to use.
  #
  # The matcher runs on the position record and pairs the other two by arrival
  # age, so these are how a change to the receiver's output configuration shows
  # up: disable velocity, or move it to a rate far below position's, and this
  # climbs instead of the matcher silently falling back to distance alone.
  # Against a receiver sending all three together, both stay near zero.
  fixesWithoutVelocity @13 :UInt64;
  fixesWithoutSigma @14 :UInt64;
}
