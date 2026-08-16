@0x8eede9d71706d6d3;

using Common = import "map_common.capnp";

# Questions asked of the road graph: what road is here, and how do I get there.
#
# Query/reply rather than pub/sub, for the same reason as map_tiles.capnp -- a
# client asks about the one place it cares about and nothing else. The graph
# itself is built by tools/map_build and served by nodes/map_server beside the
# tile services.
#
# NOTE what does not use these: nodes/map_match. A matcher needs a candidate
# set and several bounded shortest-path probes per GNSS epoch, which is dozens
# of lookups at 10 Hz; it mmaps the graph directly instead. These services are
# for tools, for the inspector, and for clients that ask occasionally.

# What can go wrong asking the graph a question.
#
# Deliberately NOT map_tiles.capnp's MapStatus, which is frozen as the
# tile/catalog/asset vocabulary. That enum's words are about a pyramid of
# files -- `noSuchTileset`, `outOfRange` meaning outside minzoom/maxzoom -- and
# none of them mean anything here, while the answers a graph query needs have
# no equivalent there. Sharing one enum would make every consumer's exhaustive
# switch a union of cases that cannot occur for its own service, which is
# exactly what stops -Wswitch-enum being useful.
enum MapQueryStatus {
  # Never sent. Present so a default-constructed response is not silently "ok".
  unknown @0;
  ok @1;
  # Nothing within the search radius. NORMAL, and not an error: a vehicle in a
  # car park or on a private road genuinely is not on a mapped road, and a
  # client must render that rather than log it.
  noMatch @2;
  # No graph by that name is loaded on the server. Distinct from noMatch, and
  # the distinction is the whole point: one means "we looked", the other means
  # "we could not look".
  noSuchGraph @3;
  # The query point is outside the graph's coverage. Also distinct from
  # noMatch -- a driver past the edge of the map should be told so.
  outOfCoverage @4;
  # Both endpoints matched and no path connects them under this cost profile.
  noRoute @5;
  # The request could not be understood -- a radius of zero, an unknown profile.
  badRequest @6;
  # The server tried and failed. The one worth a log line.
  failed @7;
}

# ============================================================================
# What road is here
# ============================================================================

# One candidate road for a position.
#
# `segmentId` is the identity everything else in this tree uses for a road: the
# horizon reports it, the router's edges carry it, and (eventually) the vector
# tiles stamp it, so a client can join across all three. It is derived from the
# source data rather than from an array index, so it survives a rebuild --
# see libs/road_graph.
struct MapSegmentMatch {
  where @0 :Common.MapSegmentRef;

  # The OSM way this came from. Present so an answer can always be checked
  # against the source, which is how every test in this path is anchored.
  osmWayId @1 :UInt64;

  # Perpendicular distance from the queried point to the segment geometry.
  # NOT an error estimate -- OSM road geometry is routinely several metres from
  # the real centreline, and with an RTK fix the receiver is the more accurate
  # of the two.
  distanceM @2 :Float32;

  # The segment's own bearing at `where`, degrees clockwise from true north.
  # A caller comparing this against a course-over-ground has to allow for
  # `where.forward`: a segment traversed backwards bears the opposite way.
  headingDeg @3 :Float32;

  name @4 :Text;
  # "I-405", "CA-73". Separate from name because a road usually has both and a
  # display wants to choose.
  ref @5 :Text;

  roadClass @6 :Common.MapRoadClass;
  # The raw OSM `highway` value. The escape hatch for a road this build has no
  # MapRoadClass word for, on the gsof_common.capnp principle: a receiver newer
  # than the reader still puts its answer on the bus.
  highwayTag @7 :Text;

  speed @8 :Common.MapSpeed;
}

struct MapNearestRequest {
  # The graph name from the server's YAML, not a file path -- same rule as a
  # tileset name.
  graph @0 :Text;

  latitudeDeg @1 :Float64;
  longitudeDeg @2 :Float64;

  # How far to look. Zero is badRequest rather than a default, because a
  # forgotten field must not silently become a 50 m search.
  radiusM @3 :Float32;

  # Optional course over ground. When `hasHeading`, candidates whose bearing
  # disagrees are ranked lower -- which is what separates a divided highway
  # from its own opposite carriageway, and a road from the frontage road
  # beside it.
  hasHeading @4 :Bool;
  headingDeg @5 :Float32;

  # Zero means "one". Ordered nearest-first after heading weighting.
  maxCandidates @6 :UInt16;
}

struct MapNearestResponse {
  status @0 :MapQueryStatus;
  # Empty unless status says otherwise.
  error @1 :Text;

  # Empty when status is not ok. Note this is NOT the same as noMatch, which is
  # a status: an empty list with status ok would be ambiguous.
  candidates @2 :List(MapSegmentMatch);
}

# ============================================================================
# Routing
#
# Declared now, answered from stage 4 onwards. Present in the schema early so
# the service key, the status vocabulary and the reply shape are settled before
# anything binds to them.
# ============================================================================

struct MapRouteRequest {
  graph @0 :Text;

  fromLatitudeDeg @1 :Float64;
  fromLongitudeDeg @2 :Float64;
  toLatitudeDeg @3 :Float64;
  toLongitudeDeg @4 :Float64;

  # A departure heading, so a route does not begin with a U-turn the driver
  # cannot make.
  hasFromHeading @5 :Bool;
  fromHeadingDeg @6 :Float32;

  # Which precomputed cost profile to use -- "fastest", "shortest",
  # "avoid_tolls". Empty means the graph's default. Profiles are built
  # offline and are weight-only: none of them removes edges, because a profile
  # that changed the topology could not share preprocessing with the others.
  profile @7 :Text;

  # Drop geometry points that lie within this many metres of the line they sit
  # on. Zero keeps every point.
  #
  # A route is drawn at a zoom, and at z12 a 40 km route's centimetre-accurate
  # geometry is thousands of points inside one screen pixel. The caller knows
  # its zoom and the server knows the geometry, so the caller says how much
  # precision it needs rather than receiving all of it and throwing most away.
  # Segment boundaries survive simplification regardless, or segmentStarts
  # would stop lining up.
  simplifyToleranceM @8 :Float32;
}

struct MapRouteResponse {
  status @0 :MapQueryStatus;
  error @1 :Text;

  # The segments traversed, in order. A client that has the graph can render
  # from these alone; one that does not uses `geometry`.
  segmentIds @2 :List(UInt64);

  # Interleaved longitude, latitude pairs -- lon first, matching
  # MapTileset.bounds -- in 1e-7 DEGREES, which is what the graph itself
  # stores. Float64 was two words per coordinate to carry a number that had
  # seven decimal places in it: a 40 km route is thousands of points, and half
  # of every one of them was zeroes.
  #
  # 1e-7 degrees is about 11 mm at the equator, far finer than OSM centrelines
  # are surveyed. This will still NOT lie exactly on top of the road as drawn
  # from vector tiles at low zoom, where tile geometry is simplified and
  # quantised -- drawing the tile feature by segmentId is the exact answer and
  # this is the portable one.
  geometry @3 :List(Int32);

  # Where each segment's geometry starts, as an index into `geometry` in
  # COORDINATE PAIRS -- so segment i occupies
  # [segmentStarts[i], segmentStarts[i+1]) and there is one more entry than
  # there are segments.
  #
  # Without this a client holding both lists cannot say which points belong to
  # which segment, so it can draw the line or label the roads but not both --
  # and "highlight the segment I am on" is the first thing a route overlay
  # wants.
  segmentStarts @6 :List(UInt32);

  distanceM @4 :Float64;
  durationS @5 :Float64;
}

# ============================================================================
# What the server has
# ============================================================================

struct MapGraphInfo {
  name @0 :Text;

  # [west, south, east, north] in degrees, as MapTileset.bounds. Empty when the
  # graph failed to open, which is how a client tells "outside coverage" from
  # "no coverage loaded".
  bounds @1 :List(Float64);

  open @2 :Bool;
  # Empty unless the graph failed to open.
  error @3 :Text;

  segmentCount @4 :UInt64;
  edgeCount @5 :UInt64;

  # Which cost profiles have precomputed overlays. A route request naming one
  # that is not here is badRequest.
  profiles @6 :List(Text);

  # When the source extract was built, as Unix seconds. Zero if unknown. A
  # driver reporting a missing road is asking about this number.
  builtAtUnixS @7 :UInt64;
}

# Empty `graph` asks for every one of them, as MapCatalogRequest does.
struct MapGraphInfoRequest {
  graph @0 :Text;
}

struct MapGraphInfoResponse {
  status @0 :MapQueryStatus;
  error @1 :Text;
  graphs @2 :List(MapGraphInfo);
}
