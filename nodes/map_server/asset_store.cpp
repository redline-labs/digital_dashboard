// SPDX-License-Identifier: GPL-3.0-or-later

#include "asset_store.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>

namespace map_server
{
namespace
{

// Lower-case the extension so ".PBF" and ".pbf" get the same answer. Only ASCII
// matters here: these are file extensions in a directory we ship.
std::string lowerExtension(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

} // namespace

const char* to_string(AssetStatus status)
{
    switch (status)
    {
        case AssetStatus::Ok:
            return "ok";
        case AssetStatus::NotFound:
            return "not found";
        case AssetStatus::Rejected:
            return "rejected";
        case AssetStatus::TooLarge:
            return "too large";
        case AssetStatus::ReadFailed:
            return "read failed";
        case AssetStatus::Disabled:
            return "disabled";
    }

    return "unknown";
}

std::string contentTypeFor(const std::filesystem::path& path)
{
    const std::string ext = lowerExtension(path);

    if (ext == ".json")
    {
        return "application/json";
    }
    if (ext == ".pbf")
    {
        // What a glyph range is. No client reads this field, but a human
        // looking at the bus does.
        return "application/x-protobuf";
    }
    if (ext == ".png")
    {
        return "image/png";
    }
    if (ext == ".jpg" || ext == ".jpeg")
    {
        return "image/jpeg";
    }
    if (ext == ".webp")
    {
        return "image/webp";
    }
    if (ext == ".svg")
    {
        return "image/svg+xml";
    }

    // Not a guess. A wrong content type on a style JSON is worse than an
    // unhelpful one.
    return "application/octet-stream";
}

AssetStore::AssetStore(const std::filesystem::path& root, std::uint64_t maxBytes) :
    mMaxBytes(maxBytes)
{
    if (root.empty())
    {
        return;
    }

    std::error_code ec;
    // canonical, not weakly_canonical: the root has to exist for containment to
    // mean anything. Comparing against a path with unresolved symlinks in it
    // would compare two different spellings of the same directory and refuse
    // everything.
    mRoot = std::filesystem::canonical(root, ec);
    if (ec || !std::filesystem::is_directory(mRoot, ec))
    {
        SPDLOG_WARN("[assets] root '{}' is not a readable directory; the asset service is off",
                    root.string());
        mRoot.clear();
        return;
    }

    mEnabled = true;
}

std::filesystem::path AssetStore::resolve(std::string_view requestPath) const
{
    if (!mEnabled || requestPath.empty())
    {
        return {};
    }

    const std::filesystem::path requested { std::string(requestPath) };

    // Absolute paths and Windows-style roots never make sense here: the request
    // is always relative to the asset root.
    if (requested.is_absolute() || requested.has_root_name())
    {
        return {};
    }

    // Refuse ".." outright, before touching the filesystem. weakly_canonical
    // below would catch most of it, but a path that climbs out and back in
    // ("../assets/style.json" from a sibling root) resolves to something inside
    // and is still not a request this store should honour -- it means the
    // client is reasoning about the server's directory layout.
    for (const auto& part : requested)
    {
        if (part == "..")
        {
            return {};
        }
    }

    std::error_code ec;
    const std::filesystem::path candidate = std::filesystem::weakly_canonical(mRoot / requested, ec);
    if (ec)
    {
        return {};
    }

    // The containment check itself. Comparing iterator by iterator rather than
    // with a string prefix, because "/maps/assets-evil" starts with
    // "/maps/assets" as text and is a different directory.
    auto rootPart = mRoot.begin();
    auto candidatePart = candidate.begin();
    for (; rootPart != mRoot.end(); ++rootPart, ++candidatePart)
    {
        if (candidatePart == candidate.end() || *candidatePart != *rootPart)
        {
            return {};
        }
    }

    return candidate;
}

AssetStatus AssetStore::load(std::string_view requestPath, Asset& out) const
{
    if (!mEnabled)
    {
        return AssetStatus::Disabled;
    }

    const std::filesystem::path path = resolve(requestPath);
    if (path.empty())
    {
        return AssetStatus::Rejected;
    }

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec)
    {
        return AssetStatus::NotFound;
    }
    if (!std::filesystem::is_regular_file(path, ec) || ec)
    {
        // A directory is not an asset. Reported as NotFound rather than
        // Rejected: the path was legitimate, there is just nothing to send.
        return AssetStatus::NotFound;
    }

    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec)
    {
        return AssetStatus::ReadFailed;
    }
    if (size > mMaxBytes)
    {
        SPDLOG_WARN("[assets] '{}' is {} bytes, over the {} byte ceiling", path.string(), size,
                    mMaxBytes);
        return AssetStatus::TooLarge;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return AssetStatus::ReadFailed;
    }

    out.data.resize(static_cast<std::size_t>(size));
    if (size > 0)
    {
        file.read(reinterpret_cast<char*>(out.data.data()), static_cast<std::streamsize>(size));
        if (!file)
        {
            out.data.clear();
            return AssetStatus::ReadFailed;
        }
    }

    out.contentType = contentTypeFor(path);
    out.encoding = mbtiles::sniff(out.data);
    return AssetStatus::Ok;
}

} // namespace map_server
