// SPDX-License-Identifier: GPL-3.0-or-later
//
// The asset store's containment check.
//
// This is the only place in the node that turns text off the bus into a
// filesystem path, so it is the only place that can be talked out of the
// directory it was given. A style JSON is fetched by URL, and a URL is written
// by whoever wrote the style -- so "the request comes from our own style file"
// is not a property this code may rely on.
//
// The traversal cases are the obvious half. The one that is easy to get wrong
// is the SIBLING: "/maps/assets-evil" begins with "/maps/assets" as a string,
// so a containment check written as a string prefix lets it through. This one
// compares path components, and that case is pinned below.

#include "asset_store.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

using map_server::Asset;
using map_server::AssetStatus;
using map_server::AssetStore;

// A directory tree that deletes itself:
//
//   <tmp>/root/style.json
//   <tmp>/root/fonts/Noto/0-255.pbf
//   <tmp>/root/sub/           (a directory, not a file)
//   <tmp>/outside/secret.txt          <- must never be reachable
//   <tmp>/root-evil/secret.txt        <- the sibling-prefix case
//   <tmp>/root/escape         -> <tmp>/outside   (symlink)
class Fixture
{
  public:
    Fixture()
    {
        std::error_code ec;
        mBase = std::filesystem::temp_directory_path(ec) /
                ("map_server_assets_" + std::to_string(::getpid()));
        std::filesystem::remove_all(mBase, ec);

        std::filesystem::create_directories(mBase / "root" / "fonts" / "Noto", ec);
        std::filesystem::create_directories(mBase / "root" / "sub", ec);
        std::filesystem::create_directories(mBase / "outside", ec);
        std::filesystem::create_directories(mBase / "root-evil", ec);

        write(mBase / "root" / "style.json", R"({"version":8})");
        write(mBase / "root" / "fonts" / "Noto" / "0-255.pbf", "\x1f\x8b\x08\x00glyphs");
        write(mBase / "root" / "big.bin", std::string(4096, 'x'));
        write(mBase / "root" / "mystery.xyz", "unknown extension");
        write(mBase / "outside" / "secret.txt", "you should not see this");
        write(mBase / "root-evil" / "secret.txt", "nor this");

        std::filesystem::create_directory_symlink(mBase / "outside", mBase / "root" / "escape", ec);
        mSymlinked = !ec;
    }

    ~Fixture()
    {
        std::error_code ec;
        std::filesystem::remove_all(mBase, ec);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    std::filesystem::path root() const { return mBase / "root"; }
    std::filesystem::path base() const { return mBase; }
    bool symlinked() const { return mSymlinked; }

  private:
    static void write(const std::filesystem::path& path, const std::string& contents)
    {
        std::ofstream out(path, std::ios::binary);
        out << contents;
    }

    std::filesystem::path mBase;
    bool mSymlinked { false };
};

// ============================================================================

void test_an_asset_inside_the_root_is_served()
{
    const Fixture fixture;
    const AssetStore store(fixture.root(), 8U * 1024U * 1024U);
    check(store.enabled(), "a real directory enables the store");

    Asset asset;
    check(store.load("style.json", asset) == AssetStatus::Ok, "style.json is served");
    check(!asset.data.empty(), "and has contents");
    check(asset.contentType == "application/json", "with the right content type");
    check(asset.encoding == mbtiles::Encoding::Identity, "and no compression claimed");

    Asset glyphs;
    check(store.load("fonts/Noto/0-255.pbf", glyphs) == AssetStatus::Ok,
          "a nested glyph range is served");
    check(glyphs.contentType == "application/x-protobuf", "a .pbf is protobuf");
    check(glyphs.encoding == mbtiles::Encoding::Gzip,
          "a gzipped glyph range is reported as gzip and passed through");
}

void test_traversal_is_refused()
{
    const Fixture fixture;
    const AssetStore store(fixture.root(), 8U * 1024U * 1024U);

    Asset asset;
    check(store.load("../outside/secret.txt", asset) == AssetStatus::Rejected,
          "a .. traversal is refused");
    check(store.load("../../etc/passwd", asset) == AssetStatus::Rejected,
          "a deeper traversal is refused");
    check(store.load("fonts/../../outside/secret.txt", asset) == AssetStatus::Rejected,
          "a traversal through a real subdirectory is refused");

    // Climbs out and back in, so it resolves to something inside the root. Still
    // refused: a client spelling paths relative to the server's parent
    // directory is reasoning about a layout it should not know.
    check(store.load("../root/style.json", asset) == AssetStatus::Rejected,
          "a path that climbs out and back in is refused");
}

void test_absolute_paths_are_refused()
{
    const Fixture fixture;
    const AssetStore store(fixture.root(), 8U * 1024U * 1024U);

    Asset asset;
    check(store.load("/etc/passwd", asset) == AssetStatus::Rejected,
          "an absolute path is refused");

    // Even an absolute path that IS inside the root. The request grammar is
    // relative; accepting both spellings means two code paths to keep correct.
    const std::string inside = (fixture.root() / "style.json").string();
    check(store.load(inside, asset) == AssetStatus::Rejected,
          "an absolute path inside the root is refused too");
}

void test_a_symlink_out_of_the_root_is_refused()
{
    const Fixture fixture;
    if (!fixture.symlinked())
    {
        SPDLOG_WARN("symlink could not be created; skipping the symlink case");
        return;
    }

    const AssetStore store(fixture.root(), 8U * 1024U * 1024U);

    // The path has no .. in it and every component exists. Only resolving the
    // link and re-checking containment catches this.
    Asset asset;
    check(store.load("escape/secret.txt", asset) == AssetStatus::Rejected,
          "a symlink pointing outside the root is refused");
}

void test_a_sibling_directory_sharing_a_prefix_is_refused()
{
    // "<base>/root-evil" starts with "<base>/root" as a string. A containment
    // check written with string::starts_with would serve this.
    const Fixture fixture;
    const AssetStore store(fixture.root(), 8U * 1024U * 1024U);

    Asset asset;
    check(store.load("../root-evil/secret.txt", asset) == AssetStatus::Rejected,
          "a sibling directory sharing a name prefix is refused");

    // And directly, in case the .. rule is ever relaxed: resolve() is the
    // function the rule lives in.
    check(store.resolve("../root-evil/secret.txt").empty(),
          "resolve() refuses the prefix sibling outright");
}

void test_a_missing_file_is_not_found_not_rejected()
{
    // The distinction matters: NotFound means the style asked for something
    // that is not deployed, Rejected means it asked for something it should
    // not. The node counts them separately for exactly that reason.
    const Fixture fixture;
    const AssetStore store(fixture.root(), 8U * 1024U * 1024U);

    Asset asset;
    check(store.load("no-such-style.json", asset) == AssetStatus::NotFound,
          "a missing file inside the root is NotFound");
    check(store.load("fonts/Nope/0-255.pbf", asset) == AssetStatus::NotFound,
          "a missing nested file is NotFound");
}

void test_a_directory_is_not_an_asset()
{
    const Fixture fixture;
    const AssetStore store(fixture.root(), 8U * 1024U * 1024U);

    Asset asset;
    check(store.load("sub", asset) == AssetStatus::NotFound, "a directory is not an asset");
    check(store.load("", asset) == AssetStatus::Rejected, "an empty path is refused");
}

void test_the_size_ceiling_is_enforced()
{
    const Fixture fixture;
    const AssetStore store(fixture.root(), 1024);

    Asset asset;
    check(store.load("big.bin", asset) == AssetStatus::TooLarge,
          "a file over the ceiling is refused");
    check(store.load("style.json", asset) == AssetStatus::Ok,
          "a file under the ceiling is still served");
}

void test_an_unknown_extension_is_not_guessed()
{
    const Fixture fixture;
    const AssetStore store(fixture.root(), 8U * 1024U * 1024U);

    Asset asset;
    check(store.load("mystery.xyz", asset) == AssetStatus::Ok, "an unknown extension is served");
    check(asset.contentType == "application/octet-stream",
          "and is described as octet-stream rather than guessed");
}

void test_a_missing_root_disables_rather_than_crashes()
{
    const Fixture fixture;

    const AssetStore missing(fixture.base() / "no-such-directory", 4096);
    check(!missing.enabled(), "a nonexistent root leaves the store disabled");

    Asset asset;
    check(missing.load("style.json", asset) == AssetStatus::Disabled,
          "a disabled store reports Disabled rather than NotFound");

    const AssetStore unset(std::filesystem::path {}, 4096);
    check(!unset.enabled(), "an empty root leaves the store disabled");
    check(unset.load("style.json", asset) == AssetStatus::Disabled,
          "an unset root reports Disabled");
}

} // namespace

int main()
{
    // `err`, not `critical`: the store logs its refusals at WARN and this test
    // provokes a lot of them, but a suppressed FAIL line is a test that reports
    // a count and not what broke.
    spdlog::set_level(spdlog::level::err);
    spdlog::set_pattern("[%^%l%$] %v");

    test_an_asset_inside_the_root_is_served();
    test_traversal_is_refused();
    test_absolute_paths_are_refused();
    test_a_symlink_out_of_the_root_is_refused();
    test_a_sibling_directory_sharing_a_prefix_is_refused();
    test_a_missing_file_is_not_found_not_rejected();
    test_a_directory_is_not_an_asset();
    test_the_size_ceiling_is_enforced();
    test_an_unknown_extension_is_not_guessed();
    test_a_missing_root_disables_rather_than_crashes();

    spdlog::set_level(spdlog::level::info);

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all map_server asset checks passed");
    return 0;
}
