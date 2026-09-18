// The page's own files: index.html, app.js, style.css, redline.js/.wasm.
//
// WHY NOT httplib's set_mount_point. It validates caches by file mtime (ETag
// "mtime-size", Last-Modified, If-Modified-Since), and on the image every file
// carries the same mtime -- the build's SOURCE_DATE_EPOCH, 2011 -- in every
// release. A browser then treats a file "fifteen years old" as fresh for a year
// or more without asking, and when it does ask, If-Modified-Since is answered
// 304 forever. After an update the page ran the previous release's app.js
// against the new server.
//
// So the validator is the content: a strong ETag hashed from the bytes, with
// Cache-Control: no-cache so every load asks. An unchanged file still costs
// only a 304, which matters for the 1.5 MB wasm module.
#ifndef WEB_CONSOLE_STATIC_ASSETS_H_
#define WEB_CONSOLE_STATIC_ASSETS_H_

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace web_console
{

struct Asset
{
    std::string body;
    std::string contentType;
    std::string etag;  // strong, quoted: "\"<16 hex digits>\""
};

// The file `urlPath` names under `root`, or nullopt when there is none or it
// would resolve outside `root` ("..", an absolute path, a symlink out). An
// empty path or one ending in '/' means index.html in that directory.
std::optional<Asset> loadAsset(const std::filesystem::path& root, std::string_view urlPath);

// A strong ETag for these bytes.
std::string contentEtag(std::string_view bytes);

// Whether an If-None-Match header value matches `etag`: "*", or any entry of
// a comma-separated list, weak (W/) entries compared by their opaque part as
// RFC 9110 requires for If-None-Match.
bool ifNoneMatchHits(std::string_view ifNoneMatch, std::string_view etag);

// Content-Type by extension. application/wasm matters: without it browsers
// refuse WebAssembly.instantiateStreaming.
std::string contentTypeFor(const std::filesystem::path& file);

}  // namespace web_console

#endif  // WEB_CONSOLE_STATIC_ASSETS_H_
