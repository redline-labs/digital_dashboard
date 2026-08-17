@0xb3f1c47a9e8d2056;

using Tiles = import "map_tiles.capnp";

# Race tracks: what nodes/map_server knows about the 994 circuits in the track
# layer, beyond the tiles it draws them from.
#
# TWO SERVICES, both query/reply, for the same reason the tile service is: a
# catalogue is pulled, not pushed, and nobody wants 994 tracks arriving at 10 Hz.
#
# WHY THIS EXISTS AT ALL, given the tracks are already tiles. Tiles are lossy by
# design -- simplified per zoom, clipped to a tile, stripped of anything a
# renderer does not need. Everything that is eventually going to be built on
# this layer needs the opposite: the full-resolution outline, a centreline with
# a distance along it, and the start/finish gate. None of those survive tiling,
# and reassembling a circuit from four clipped tiles to measure a lap against it
# is not a thing anybody should attempt.
#
# Nothing in the dashboard consumes the lap-shaped half of this yet. It is on
# the wire now so that whatever eventually does is not blocked behind a
# re-ingest and a schema change -- see docs/tracks.md.

# Why a track has no usable centreline.
#
# Present because ABSENCE NEEDS A REASON. About a fifth of the corpus has no
# centreline, for five different reasons, and "hasCenterline is false" on its
# own sends somebody looking for a bug in the server. These are the ingest's
# verdicts, carried through unchanged.
enum MapTrackQuality {
  unknown @0;
  ok @1;
  # The trace never comes back to its start: a point-to-point hillclimb, which
  # has two ends rather than one. Fourteen of these.
  seamNotFound @2;
  # Several loops concatenated into one file -- an oval and its infield, or a
  # "Combo" layout. There is no single centreline to extract. Twenty-four.
  multipleLoops @3;
  # The two boundaries paired into a "track" hundreds of metres wide.
  widthOutOfRange @4;
  # The centreline disagrees with the published lap length by more than the
  # ingest's tolerance.
  lengthMismatch @5;
  # The OUTLINE disagrees with the published lap length, before any centreline
  # was derived. A source-data problem, deliberately distinct from the two
  # above so it is not read as our arithmetic going wrong.
  sourceLengthImplausible @6;
  degenerate @7;
}

enum MapGateSource {
  none @0;
  # The source GeoJSON's own Start / Finish point.
  dataDrop @1;
  # Worked out from the geometry because the source had none.
  derived @2;
  manual @3;
}

# The start/finish line.
#
# A CENTRE AND TWO ENDS, not a point. A gate is a line a vehicle crosses, and a
# crossing test against a point alone is a distance threshold -- at 250 km/h a
# 10 Hz fix moves 7 m between samples, so a threshold either fires late, fires
# twice, or misses entirely on the lap somebody actually cared about.
struct MapStartFinishGate {
  source @0 :MapGateSource;

  centreLatE7 @1 :Int32;
  centreLonE7 @2 :Int32;
  leftLatE7 @3 :Int32;
  leftLonE7 @4 :Int32;
  rightLatE7 @5 :Int32;
  rightLonE7 @6 :Int32;

  # How far the centreline's first sample sits from the point the source gave.
  # Under one sample spacing by construction; carried so a consumer knows the
  # lap does not begin at a rounded-off place by accident.
  centerlineOffsetCm @7 :UInt32;

  widthM @8 :Float32;
}

struct MapTrackSummary {
  # The source file stem. STABLE ACROSS REBUILDS, which is why it is the
  # filename and not an index, a geometry hash, or the display name -- two
  # files are called nothing at all and several share a `circuit`.
  id @0 :Text;
  name @1 :Text;
  # The canonical circuit name, where the source gives one. Several layouts of
  # one venue share it, so it is NOT an identity.
  circuit @2 :Text;

  # The largest layout at the same place, for the layouts that overlap on the
  # ground -- Buttonwillow has fifteen, Buenos Aires twenty-three.
  #
  # NOT STABLE ACROSS REBUILDS. It names whichever member happened to be
  # largest among the files present at build time, so a client may use it
  # within one reply and MUST NOT persist it. That is what `buildId` is for.
  venueId @3 :Text;

  # [west, south, east, north] in degrees -- the order MapTileset.bounds and
  # the mbtiles metadata already use.
  bounds @4 :List(Float64);

  centerlineLengthM @5 :Float64;
  # From the source's own Start / Finish point. ZERO MEANS THE FILE DID NOT
  # SAY -- 34 of them do not -- and is NOT a claim that the lap is zero long,
  # nor that this and centerlineLengthM agree.
  publishedLengthM @6 :Float64;
  medianWidthM @7 :Float64;
  # The long axis of the outline, degrees clockwise from north. An AXIS and not
  # a direction: 190 and 10 describe the same line. A viewer rotating a circuit
  # to fill a wide screen wants this, and it is a property of the geometry
  # rather than of the view, so it is computed once at ingest.
  principalAxisDeg @8 :Float64;

  # USABLE, not merely present. A rejected track keeps an outline and gets no
  # centreline, and nothing may measure a lap against one that failed the gate.
  hasCenterline @9 :Bool;
  closed @10 :Bool;
  # The source marks 33 layouts as combinations of others.
  combo @11 :Bool;
  quality @12 :MapTrackQuality;
  outlinePoints @13 :UInt32;

  gate @14 :MapStartFinishGate;
}

# An empty request asks for everything, which is what a catalogue load wants.
# `near` narrows it to what is in range, which is what a picker wants.
struct MapTrackCatalogRequest {
  # The name from the server's YAML. Empty means the first one configured,
  # which is the common case -- there is only ever one track layer.
  trackset @0 :Text;

  venueId @1 :Text;
  # [lon, lat]. Empty for no filter. LON FIRST, as everywhere else on this bus.
  near @2 :List(Float64);
  radiusM @3 :Float64;
}

struct MapTrackCatalogResponse {
  status @0 :Tiles.MapStatus;
  error @1 :Text;

  # Identifies the ARTIFACT these ids came from, on EVERY reply.
  #
  # A consumer holding geometry from one build and a catalogue from another
  # measures against the wrong centreline and renders perfectly. venueId is not
  # stable across builds either, so this is what makes it safe to use at all.
  buildId @2 :Text;

  tracks @3 :List(MapTrackSummary);
}

struct MapTrackDetailRequest {
  trackset @0 :Text;
  id @1 :Text;

  wantOutline @2 :Bool;
  wantCenterline @3 :Bool;

  # Douglas-Peucker before sending, in metres. The caller knows its zoom and
  # the server knows the geometry, so the caller says how much detail it can
  # use and the server does the work once.
  #
  # ZERO SENDS EVERYTHING, which for Milford Road Course is 55 000 points --
  # 440 kB on the wire to draw a shape a few hundred pixels across. The server
  # clamps this to a floor rather than honouring a zero from a client that did
  # not think about it.
  simplifyToleranceM @4 :Float32;
}

struct MapTrackDetailResponse {
  status @0 :Tiles.MapStatus;
  error @1 :Text;
  buildId @2 :Text;
  summary @3 :MapTrackSummary;

  # Interleaved LONGITUDE, LATITUDE in 1e-7 degrees -- lon first, exactly as
  # MapRouteResponse.geometry. The source files are GeoJSON, which is also lon
  # first; the fixed-point convention in between is this tree's, and the swap
  # happens at ingest and nowhere else.
  outlineOuter @4 :List(Int32);
  # The infield boundary, i.e. the HOLE. Empty when the ingest found no seam,
  # in which case the outline is a solid ribbon rather than a loop.
  outlineInner @5 :List(Int32);
  centerline @6 :List(Int32);

  # Parallel to `centerline`, one entry per POINT: distance from the gate, and
  # half the local track width. Empty when hasCenterline is false.
  #
  # These two lists plus the gate are the whole reason this service exists.
  # Everything a consumer needs to turn a GNSS fix into a distance along the
  # lap is here, indexed by DISTANCE rather than by time -- which is what makes
  # a lap comparable against another lap driven at a different speed.
  centerlineDistanceCm @7 :List(UInt32);
  halfWidthCm @8 :List(UInt16);
}
