// SPDX-License-Identifier: GPL-3.0-or-later
//
// Files served beside the tiles: the style JSON, glyph ranges, sprite sheets.
//
// Split out of the service so the part worth testing -- deciding whether a
// requested path is inside the asset root -- is reachable without a zenoh
// session. It is the only place in this node that turns text from the bus into
// a filesystem path, so it is the only place that can be talked out of the
// directory it was given.
#ifndef MAP_SERVER_ASSET_STORE_H
#define MAP_SERVER_ASSET_STORE_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "mbtiles/compression.h"

namespace map_server
{

enum class AssetStatus
{
    Ok,
    // The path is fine and there is no such file.
    NotFound,
    // The path is not one this store will resolve: empty, absolute, or one that
    // climbs out of the root. Distinct from NotFound on purpose -- a client
    // that asked for something outside the root has a bug or worse, and the
    // node counts it separately.
    Rejected,
    // The file is larger than the configured ceiling.
    TooLarge,
    // The file is there and could not be read.
    ReadFailed,
    // No asset root is configured, so the service is off.
    Disabled,
};

const char* to_string(AssetStatus status);

struct Asset
{
    std::vector<std::uint8_t> data;
    std::string contentType;
    // Glyph ranges are usually stored gzipped and are passed through untouched,
    // for the same reason tiles are.
    mbtiles::Encoding encoding { mbtiles::Encoding::Identity };
};

class AssetStore
{
  public:
    // An empty or nonexistent root leaves the store Disabled rather than
    // failing to construct: a raster-only deployment has no assets, and a
    // missing directory should say so once per request rather than stop the
    // node from serving tiles.
    AssetStore(const std::filesystem::path& root, std::uint64_t maxBytes);

    bool enabled() const { return mEnabled; }
    const std::filesystem::path& root() const { return mRoot; }

    AssetStatus load(std::string_view requestPath, Asset& out) const;

    // Exposed for the test, and because "would this path be served?" is a
    // question worth answering without touching the disk.
    //
    // Returns an empty path when the request must be refused.
    std::filesystem::path resolve(std::string_view requestPath) const;

  private:
    std::filesystem::path mRoot;
    std::uint64_t mMaxBytes;
    bool mEnabled { false };
};

// The Content-Type a client should be told, from the extension. Unknown
// extensions get application/octet-stream rather than a guess.
std::string contentTypeFor(const std::filesystem::path& path);

} // namespace map_server

#endif // MAP_SERVER_ASSET_STORE_H
