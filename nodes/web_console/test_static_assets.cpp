// The page's files: which path resolves to which file, never one outside the
// root, and a cache validator that follows the bytes rather than the mtime.

#include "static_assets.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace web_console;

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

void write(const fs::path& path, const std::string& content)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

struct Tree
{
    fs::path top = fs::temp_directory_path() / ("web_console_assets_" + std::to_string(::getpid()));
    fs::path root = top / "web";

    Tree()
    {
        fs::remove_all(top);
        write(root / "index.html", "<html>index</html>");
        write(root / "app.js", "console.log(1);");
        write(root / "redline.wasm", std::string("\0asm", 4));
        write(root / "sub" / "index.html", "sub index");
        // Outside the root, and a sibling whose name has the root as a prefix.
        write(top / "secret.txt", "secret");
        write(top / "web-old" / "app.js", "old");
        fs::create_symlink(top / "secret.txt", root / "link-out");
    }
    ~Tree() { fs::remove_all(top); }
};

void testResolution(const Tree& t)
{
    const auto index = loadAsset(t.root, "/");
    check(index && index->body == "<html>index</html>", "/ is index.html");
    check(index && index->contentType.starts_with("text/html"), "index.html is text/html");
    check(loadAsset(t.root, "") && loadAsset(t.root, "")->body == index->body, "empty path is index.html");

    const auto js = loadAsset(t.root, "/app.js");
    check(js && js->body == "console.log(1);", "/app.js");
    check(js && js->contentType.starts_with("text/javascript"), "app.js is text/javascript");

    const auto wasm = loadAsset(t.root, "/redline.wasm");
    check(wasm && wasm->contentType == "application/wasm", "wasm is application/wasm");
    check(wasm && wasm->body.size() == 4, "binary body kept whole, NUL included");

    check(loadAsset(t.root, "/sub/") && loadAsset(t.root, "/sub/")->body == "sub index",
          "a trailing slash is that directory's index.html");
    check(!loadAsset(t.root, "/missing.js"), "a missing file is nothing");
    check(!loadAsset(t.root, "/sub"), "a directory without the slash is not a file");
}

void testNothingOutsideTheRoot(const Tree& t)
{
    check(!loadAsset(t.root, "/../secret.txt"), "..");
    check(!loadAsset(t.root, "/sub/../../secret.txt"), "nested ..");
    check(!loadAsset(t.root, "/../web-old/app.js"), "a sibling sharing the root's name as a prefix");
    check(!loadAsset(t.root, "/link-out"), "a symlink leading out");
    check(!loadAsset(t.root, "//" + (t.top / "secret.txt").string()), "an absolute path");
    check(!loadAsset(t.root, std::string("/app.js\0.html", 13)), "an embedded NUL");
    check(!loadAsset(t.top / "nonexistent", "/app.js"), "a root that does not exist");
}

void testEtagFollowsContent(const Tree& t)
{
    const auto before = loadAsset(t.root, "/app.js");
    // Same size, same mtime: exactly the change httplib's mtime-size ETag and
    // Last-Modified could not see on an image built with a fixed epoch.
    const auto mtime = fs::last_write_time(t.root / "app.js");
    write(t.root / "app.js", "console.log(2);");
    fs::last_write_time(t.root / "app.js", mtime);
    const auto after = loadAsset(t.root, "/app.js");

    check(before && after && before->etag != after->etag, "same size and mtime, new content, new ETag");
    check(after && after->etag == contentEtag("console.log(2);"), "the ETag is the content's");
    check(contentEtag("x") == contentEtag("x"), "deterministic");
    check(contentEtag("").size() == 18 && contentEtag("").front() == '"' && contentEtag("").back() == '"',
          "strong and quoted");
}

void testIfNoneMatch()
{
    const std::string etag = contentEtag("abc");
    check(ifNoneMatchHits(etag, etag), "exact");
    check(ifNoneMatchHits("W/" + etag, etag), "weak form of the same tag");
    check(ifNoneMatchHits("\"other\", " + etag, etag), "in a list");
    check(ifNoneMatchHits("  " + etag + "  ", etag), "surrounding spaces");
    check(ifNoneMatchHits("*", etag), "wildcard");
    check(!ifNoneMatchHits("", etag), "absent header");
    check(!ifNoneMatchHits("\"other\"", etag), "a different tag");
    check(!ifNoneMatchHits(",,,", etag), "only separators");
    check(!ifNoneMatchHits(etag.substr(0, etag.size() - 1), etag), "a truncated tag");
    check(!ifNoneMatchHits(etag, ""), "no ETag of our own never matches");
}

}  // namespace

int main()
{
    const Tree tree;
    testResolution(tree);
    testNothingOutsideTheRoot(tree);
    testEtagFollowsContent(tree);
    testIfNoneMatch();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
