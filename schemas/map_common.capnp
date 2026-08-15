@0xed2cbc4900afb467;

# Types shared by the road-graph schemas.
#
# Same job as gsof_common.capnp: map_graph.capnp answers "what road is here"
# and map_horizon.capnp says "and here is what is ahead of you", and both have
# to name a road class and a speed limit. Two copies of those vocabularies
# would drift, and the drift is silent -- a matcher reporting `minor` and a
# horizon reporting `residential` for the same road look like two different
# roads to a consumer that switches on either.

# What kind of road this is, functionally.
#
# Deliberately short and closed. Every value here is switched somewhere, so
# `-Wswitch-enum` makes adding one a change everyone sees -- which is what we
# want for a display vocabulary and is why the long tail does NOT live here.
# A road whose OSM tagging this build has no word for arrives as `unknown`
# with the raw `highway` tag alongside it; see MapSegmentMatch.highwayTag.
enum MapRoadClass {
  unknown @0;
  motorway @1;
  trunk @2;
  primary @3;
  secondary @4;
  tertiary @5;
  # Residential and unclassified, which are the same thing to a driver.
  minor @6;
  # Driveways, parking aisles, alleys.
  service @7;
  track @8;
  path @9;
  pedestrian @10;
  ferry @11;
}

# Where a speed limit came from. A displayed limit and an assumed one are not
# the same claim, and a dash that shows 25 because nothing said otherwise is
# lying to the driver.
enum MapSpeedSource {
  unknown @0;
  # OSM records an explicit maxspeed. This is the only value that may be shown
  # to a driver as a posted limit.
  sign @1;
  # No maxspeed tag; inferred from a country/region default for built-up areas.
  implicitUrban @2;
  implicitRural @3;
  # Inferred from the road class alone -- the weakest guess we make.
  implicitClass @4;
  # A maxspeed:conditional exists and this build did not evaluate it. The
  # posted value is the unconditional one and may be wrong right now.
  conditionalIgnored @5;
}

# The two speeds, which are different numbers and must stay different fields.
#
# `posted` is what a sign says and is frequently ABSENT -- roughly half of OSM
# highway=residential has no maxspeed. A display must show nothing in that
# case rather than a default, hence `hasPosted` instead of a sentinel: zero is
# a legal speed limit in exactly the places (a barrier, a gate) where getting
# it wrong matters.
#
# `freeFlowKph` is always defined, because a router cannot cost an edge without
# one. It falls back through the class default. Never show it to a driver.
struct MapSpeed {
  hasPosted @0 :Bool;
  postedKph @1 :UInt16;
  postedSource @2 :MapSpeedSource;

  freeFlowKph @3 :UInt16;
}

# A place on the graph.
#
# `offsetCm` is measured from the segment's first geometry point in the
# segment's own direction, regardless of which way the vehicle is travelling --
# so it does not change meaning on a U-turn. `forward` says which way we are
# going along it. Centimetres in a UInt32 reach 42 949 km, which is longer than
# any segment or any horizon.
struct MapSegmentRef {
  segmentId @0 :UInt64;
  offsetCm @1 :UInt32;
  forward @2 :Bool;
}
