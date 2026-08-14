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
  # Outside the archive's minzoom/maxzoom, or x/y outside 2^z.
  outOfRange @4;
  # The request could not be understood -- an empty path, a traversal attempt.
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
struct MapTileRequest {
  # The name from the server's YAML, not a file path. Clients have no business
  # knowing where the archive lives.
  tileset @0 :Text;

  z @1 :UInt8;
  x @2 :UInt32;
  y @3 :UInt32;
}

struct MapTileResponse {
  status @0 :MapStatus;

  # Empty unless status is error or badRequest.
  error @1 :Text;

  encoding @2 :MapEncoding;

  # The archive's own `format` metadata: "pbf", "png", "jpg", "webp". Carried
  # per tile so a client that did not ask for the catalog still knows what it
  # is holding.
  format @3 :Text;

  # Empty when status is not ok. Note that this is NOT the same as a
  # zero-length tile, which is a legitimate thing for an archive to store for
  # an empty area -- status is what distinguishes them.
  data @4 :Data;
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
