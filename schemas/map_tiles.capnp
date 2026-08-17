@0xcca514be09983ce8;

# Offline maps: what nodes/map_server serves, and what the map widget asks for.
#
# Three services, all query/reply rather than pub/sub, because a map is pulled
# and not pushed -- a client asks for the handful of tiles under its viewport
# and nothing else. See docs/map.md.

enum MapStatus {
  # Never sent. Present so a default-constructed response is not silently "ok".
  unknown @0;
  ok @1;
  # The tileset exists and simply has nothing at these coordinates. Normal:
  # coverage is a bounding box at best and sparse inside it.
  notFound @2;
  # No tileset by that name is configured on the server.
  noSuchTileset @3;
  # Outside the ZOOM LEVELS THIS ARCHIVE HOLDS. Distinct from notFound, and the
  # distinction is the useful part: absence is final and the client should draw
  # nothing there, while out-of-range means the answer exists at a shallower
  # level and the client should ask for it there instead.
  outOfRange @4;
  # The request could not be understood -- an empty path, a traversal attempt,
  # or a coordinate that is not a tile at all: z past 22, or x/y outside 2^z.
  # A bug in the client, rather than a fact about the archive.
  badRequest @5;
  # The server tried and failed. See docs/map.md; this is the one worth a log
  # line. Named `failed` rather than `error` because capnp generates enumerants
  # in SCREAMING_SNAKE and ERROR is a macro on more platforms than it should be.
  failed @6;
}

# How the bytes in `data` are compressed.
#
# The server never decompresses: mbtiles stores vector tiles gzipped, the client
# has to inflate before decoding anyway, and a round trip through the server
# would cost CPU on both ends to make the payload three times larger on the
# wire. This field
# says what was stored, sniffed from the blob's magic bytes rather than trusted
# from the metadata table -- the two disagree in real archives.
enum MapEncoding {
  identity @0;
  gzip @1;
  deflate @2;
  zstd @3;
}

# ============================================================================
# Tiles
# ============================================================================

# One tile.
#
# z/x/y are SLIPPY (XYZ) coordinates -- y increasing southward, the convention
# every style document and every request on this bus uses. The mbtiles spec
# stores TMS
# rows, which count the other way. The flip between them happens exactly once,
# in mbtiles::Archive::tile(), and nowhere else. A second flip anywhere in this
# path yields a map that renders perfectly and is mirrored about the equator.
# One tile's coordinates, so a request can name several.
struct MapTileCoord {
  z @0 :UInt8;
  x @1 :UInt32;
  y @2 :UInt32;
}

# A BATCH. A viewport is a few dozen tiles and they are all wanted at once, so
# asking for them one query at a time is a few dozen round trips, a few dozen
# capnp messages and a few dozen replies to fill one screen.
#
# The list is not unbounded: a client asks for what it can draw, and a server
# that is handed thousands has been asked for something nobody will look at.
# See kMaxTilesPerRequest in the widget's tile source.
struct MapTileRequest {
  # The name from the server's YAML, not a file path. Clients have no business
  # knowing where the archive lives.
  tileset @0 :Text;

  tiles @1 :List(MapTileCoord);
}

# What became of one requested tile.
#
# PER TILE, because the answers differ within a single batch: most of the
# pyramid is empty, so a request for nine tiles routinely comes back with two
# tiles and seven notFounds, and a client that could not tell them apart would
# have to re-ask for the empty ones forever.
struct MapTileResult {
  # Echoed back. The results are in request order, but a client that pipelines
  # or retries should not have to rely on that to know what it is holding.
  coord @0 :MapTileCoord;

  status @1 :MapStatus;

  # Empty unless status is failed or badRequest.
  error @2 :Text;

  encoding @3 :MapEncoding;

  # Empty when status is not ok. Note that this is NOT the same as a
  # zero-length tile, which is a legitimate thing for an archive to store for
  # an empty area -- status is what distinguishes them.
  data @4 :Data;
}

struct MapTileResponse {
  # The answer to the REQUEST, not to any one tile: whether the tileset exists
  # and could be opened. Per-tile answers are in each result, and a request
  # that names a good tileset reports ok here even when every tile is absent.
  status @0 :MapStatus;

  # Empty unless status is error or badRequest.
  error @1 :Text;

  # The archive's own `format` metadata: "pbf", "png", "jpg", "webp". One per
  # response rather than per tile: it is a property of the archive, and it was
  # never going to differ between two tiles of the same one.
  format @2 :Text;

  tiles @3 :List(MapTileResult);

  # What the archive actually holds, on EVERY reply.
  #
  # A client cannot work this out from the per-tile answers. Most of the pyramid
  # is empty, so notFound means "nothing at this coordinate" and says nothing
  # about how deep the archive goes -- a hole over the desert at z14 and a level
  # the archive never had both come back absent. Without these a client that
  # wanted to zoom past the archive would have to walk down a level at a time,
  # one round trip each, and would still walk all the way to zero over genuinely
  # uncovered ground.
  #
  # Two bytes on a reply that already carries a hundred kilobytes of tile, and
  # it means the first reply of a session is enough: the client clamps what it
  # asks for from then on, and the outOfRange status stays the answer for the
  # one request it may get wrong before that.
  minzoom @4 :UInt8;
  maxzoom @5 :UInt8;
}

# ============================================================================
# Catalog
# ============================================================================

# What the server has. Empty `tileset` asks for every one of them, which is what
# a picker wants; naming one asks for just that entry.
struct MapCatalogRequest {
  tileset @0 :Text;
}

struct MapTileset {
  name @0 :Text;
  format @1 :Text;
  minzoom @2 :UInt8;
  maxzoom @3 :UInt8;

  # [west, south, east, north] in degrees, and [lon, lat, zoom]. Empty when the
  # archive's metadata did not say, which is allowed -- the mbtiles spec makes
  # both optional, so a client must handle absent rather than assume 0,0.
  bounds @4 :List(Float64);
  center @5 :List(Float64);

  attribution @6 :Text;
  description @7 :Text;

  # A TileJSON 2.0.0 document, already assembled from the archive's metadata
  # including the vector_layers list its `json` column carries. This is what a
  # style's `"url": "redline://catalog/<name>.json"` source resolves to, and
  # why the widget needs no other knowledge of what is in the archive.
  tileJson @8 :Text;
}

struct MapCatalogResponse {
  status @0 :MapStatus;
  error @1 :Text;
  tilesets @2 :List(MapTileset);
}

# ============================================================================
# Assets
# ============================================================================

# Everything a style needs that is not a tile: the style JSON itself, glyph
# ranges, sprite sheets.
#
# These live as files under the server's asset_root rather than in the archive,
# because they are shared across tilesets and change on a different schedule --
# restyling should not mean rebuilding a 383 MB .mbtiles.
struct MapAssetRequest {
  # Relative to the server's asset_root, always. An absolute path, or one that
  # climbs out with .., is badRequest -- see the containment check in
  # nodes/map_server.
  path @0 :Text;
}

struct MapAssetResponse {
  status @0 :MapStatus;
  error @1 :Text;

  # From the extension: application/json, application/x-protobuf, image/png.
  contentType @2 :Text;

  # Glyph ranges are usually stored gzipped, same as tiles, and are passed
  # through untouched for the same reason.
  encoding @3 :MapEncoding;

  data @4 :Data;
}

# ============================================================================
# Status
# ============================================================================

# What the server is doing. Published on a timer, so a blank map can be told
# apart from a server that never opened its archive.
struct MapServerTilesetStatus {
  name @0 :Text;
  path @1 :Text;
  open @2 :Bool;
  # Empty unless the archive failed to open.
  error @3 :Text;

  tilesServed @4 :UInt64;
  # Requests that found no tile. Expected to be non-zero and large: a client
  # asks for whatever is under the viewport, and coverage is sparse.
  tilesMissing @5 :UInt64;
  bytesServed @6 :UInt64;
}

struct MapServerStatus {
  tilesets @0 :List(MapServerTilesetStatus);

  assetRoot @1 :Text;
  assetsServed @2 :UInt64;
  assetsMissing @3 :UInt64;

  # Requests refused by the containment check. Should be zero; anything else
  # means a style is asking for something it should not, or someone is probing.
  assetsRejected @4 :UInt64;
}
