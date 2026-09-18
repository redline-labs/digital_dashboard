#include "static_assets.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <system_error>

namespace web_console
{

namespace
{

std::string_view trimSpaces(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) { s.remove_prefix(1); }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) { s.remove_suffix(1); }
    return s;
}

// weakly_canonical() then a component-wise prefix test: a string prefix would
// let /srv/web-old pass for a root of /srv/web.
bool isWithin(const std::filesystem::path& root, const std::filesystem::path& candidate)
{
    auto r = root.begin();
    auto c = candidate.begin();
    for (; r != root.end(); ++r, ++c)
    {
        if (r->empty()) { continue; }  // the trailing "" of a path ending in '/'
        if (c == candidate.end() || *r != *c) { return false; }
    }
    return true;
}

}  // namespace

std::string contentEtag(std::string_view bytes)
{
    // FNV-1a 64: a cache validator, not a security property -- it only has to
    // change when the file does.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char ch : bytes)
    {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 1099511628211ULL;
    }
    static constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5', '6', '7',
                                               '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string out = "\"";
    for (int shift = 60; shift >= 0; shift -= 4)
    {
        out += kHex[static_cast<std::size_t>((hash >> shift) & 0xFU)];
    }
    out += '"';
    return out;
}

bool ifNoneMatchHits(std::string_view ifNoneMatch, std::string_view etag)
{
    const auto opaque = [](std::string_view tag) {
        tag = trimSpaces(tag);
        if (tag.starts_with("W/")) { tag.remove_prefix(2); }
        return tag;
    };
    const std::string_view want = opaque(etag);
    if (want.empty()) { return false; }

    while (!ifNoneMatch.empty())
    {
        const auto comma = ifNoneMatch.find(',');
        const std::string_view entry = opaque(ifNoneMatch.substr(0, comma));
        if (entry == "*" || entry == want) { return true; }
        if (comma == std::string_view::npos) { break; }
        ifNoneMatch.remove_prefix(comma + 1);
    }
    return false;
}

std::string contentTypeFor(const std::filesystem::path& file)
{
    const std::string ext = file.extension().string();
    if (ext == ".html") { return "text/html; charset=utf-8"; }
    if (ext == ".js") { return "text/javascript; charset=utf-8"; }
    if (ext == ".css") { return "text/css; charset=utf-8"; }
    if (ext == ".wasm") { return "application/wasm"; }
    if (ext == ".json") { return "application/json"; }
    if (ext == ".svg") { return "image/svg+xml"; }
    if (ext == ".png") { return "image/png"; }
    if (ext == ".ico") { return "image/x-icon"; }
    return "application/octet-stream";
}

std::optional<Asset> loadAsset(const std::filesystem::path& root, std::string_view urlPath)
{
    std::string relative(urlPath);
    while (!relative.empty() && relative.front() == '/') { relative.erase(0, 1); }
    if (relative.empty() || relative.back() == '/') { relative += "index.html"; }
    // A NUL cannot name a file, and would truncate the path at the OS.
    if (relative.find('\0') != std::string::npos) { return std::nullopt; }

    std::error_code ec;
    const std::filesystem::path base = std::filesystem::weakly_canonical(root, ec);
    if (ec) { return std::nullopt; }
    const std::filesystem::path file = std::filesystem::weakly_canonical(base / relative, ec);
    if (ec || !isWithin(base, file)) { return std::nullopt; }
    if (!std::filesystem::is_regular_file(file, ec) || ec) { return std::nullopt; }

    std::ifstream in(file, std::ios::binary);
    if (!in) { return std::nullopt; }
    Asset asset;
    asset.body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) { return std::nullopt; }
    asset.contentType = contentTypeFor(file);
    asset.etag = contentEtag(asset.body);
    return asset;
}

}  // namespace web_console
