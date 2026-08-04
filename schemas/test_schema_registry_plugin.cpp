// SPDX-License-Identifier: GPL-3.0-or-later
//
// The schema registry generator, over schemas written the ways the CMake regex
// it replaced got wrong.
//
// The regex lived in schemas/CMakeLists.txt and matched
// "struct[ \t]+([a-zA-Z_][a-zA-Z0-9_]*)[ \t]" against the schema *text*. It
// dropped `struct Foo{` entirely -- silently, so the schema compiled and
// generated and simply was not in the registry until something failed to decode
// a topic at runtime -- while matching the word "struct" inside comments and
// grabbing nested structs by an unqualified name that did not compile.
//
// Each case below is one of those. The point is not that the generator works
// today; it is that these specific shapes cannot regress, because the failure
// they used to produce was invisible at build time.
//
// Runs the real `capnp compile` against the real plugin binary, both passed in
// by CMake, so nothing here re-implements the thing under test.

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        SPDLOG_ERROR("FAIL: {}", what);
    }
}

// Runs the generator over the named fixtures. Returns the process exit status;
// generated output lands in `out_dir`.
int runGenerator(const std::filesystem::path& out_dir,
                 const std::vector<std::string>& fixtures)
{
    std::filesystem::create_directories(out_dir);

    std::ostringstream command;
    command << '"' << CAPNP_TOOL << "\" compile"
            << " -o\"" << REGISTRY_PLUGIN << "\":\"" << out_dir.string() << '"'
            << " --src-prefix=\"" << FIXTURE_DIR << '"';
    for (const auto& fixture : fixtures)
    {
        command << " \"" << FIXTURE_DIR << '/' << fixture << '"';
    }
    // The duplicate-name case is *supposed* to fail, and capnp reports plugin
    // failures on stderr. Keep it out of the log so a passing run reads clean.
    command << " 2>/dev/null";

    return std::system(command.str().c_str());
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream stream(path);
    if (!stream)
    {
        return {};
    }
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

// The whole point of the rewrite: capnp parses, so formatting does not decide
// what ends up in the registry.
void testAwkwardFormatting(const std::filesystem::path& scratch)
{
    const auto out_dir = scratch / "awkward";
    const int status = runGenerator(out_dir, {"awkward.capnp"});
    expect(status == 0, "generator succeeds on awkwardly formatted schemas");
    if (status != 0)
    {
        return;
    }

    const std::string header = readFile(out_dir / "pub_sub" / "schema_registry.h");
    expect(!header.empty(), "a header was generated");

    // The silent one. `struct Tight{` has no whitespace before the brace.
    expect(contains(header, "schema_type_t::Tight") || contains(header, "    Tight,"),
           "a struct written 'struct Tight{' is registered");
    expect(contains(header, "\"Tight\""), "Tight has a registry name");

    // The word "struct Ghost" appears only inside a comment in the fixture.
    expect(!contains(header, "Ghost"), "a struct named only in a comment is not registered");

    // Nested structs are qualified, so they compile and cannot collide with a
    // top-level type of the same name.
    expect(contains(header, "Outer_Inner"), "a nested struct is registered as Outer_Inner");
    expect(contains(header, "::Outer::Inner"),
           "a nested struct resolves to its qualified C++ name");

    // A file with a nested struct cannot be forward declared, so it must be
    // included instead -- otherwise the generated header would not compile.
    expect(contains(header, "#include \"awkward.capnp.h\""),
           "a file with a nested struct is included rather than forward declared");

    // Neither an enum nor a group can be the root of a message.
    expect(!contains(header, "Outer_Mode"), "a nested enum is not registered");
    expect(!contains(header, "WithGroup_Pair") && !contains(header, "WithGroup_pair"),
           "a group is not registered");

    // The ordinary case still works.
    expect(contains(header, "\"Spacious\""), "a conventionally formatted struct is registered");
    expect(contains(header, "\"WithGroup\""), "the struct containing a group is itself registered");

    // The two halves have to agree, since one is generated from the other.
    const std::string source = readFile(out_dir / "schema_registry.cpp");
    expect(contains(source, "case schema_type_t::Tight: return capnp::Schema::from<::Tight>();"),
           "the source table has a case for Tight");
    expect(contains(source,
                    "case schema_type_t::Outer_Inner: return "
                    "capnp::Schema::from<::Outer::Inner>();"),
           "the source table qualifies the nested type");
}

// Two schemas cannot claim one name: the name is what identifies a message on
// the bus and in a dashboard config. This used to emit a duplicate enumerator
// and fail deep inside generated code, if it was noticed at all.
void testDuplicateNameIsFatal(const std::filesystem::path& scratch)
{
    const auto out_dir = scratch / "duplicate";
    const int status = runGenerator(out_dir, {"duplicate_a.capnp", "duplicate_b.capnp"});
    expect(status != 0, "a schema name claimed by two files fails the build");
    expect(!std::filesystem::exists(out_dir / "pub_sub" / "schema_registry.h"),
           "no registry is written when the name check fails");
}

}  // namespace

int main()
{
    spdlog::set_pattern("%v");

    const auto scratch =
        std::filesystem::temp_directory_path() / "schema_registry_plugin_test";
    std::filesystem::remove_all(scratch);

    testAwkwardFormatting(scratch);
    testDuplicateNameIsFatal(scratch);

    std::filesystem::remove_all(scratch);

    if (failures > 0)
    {
        SPDLOG_ERROR("{}/{} checks failed", failures, checks);
        return 1;
    }

    SPDLOG_INFO("all {} checks passed", checks);
    return 0;
}
