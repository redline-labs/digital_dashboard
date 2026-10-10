// SPDX-License-Identifier: GPL-3.0-or-later
//
// writeFileAtomically: the file is replaced whole or not at all.
//
// The failure cases are the point. A writer that truncated the target before
// discovering it could not finish is what this replaced, so every failure here
// asserts the old contents survive and no temporary is left beside them.

#include "config_codec/config_file.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace
{

namespace fs = std::filesystem;

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

std::string slurp(const fs::path& path)
{
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

std::size_t entriesIn(const fs::path& dir)
{
    std::size_t n = 0;
    for ([[maybe_unused]] const auto& entry : fs::directory_iterator(dir))
    {
        ++n;
    }
    return n;
}

}  // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path() / ("config_file_test_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path file = dir / "layout.yaml";

    // A new file: written, and readable by others.
    expect(!config_codec::writeFileAtomically(file.string(), "a: 1\n"), "writes a new file");
    expect(slurp(file) == "a: 1\n", "with exactly the contents given");
    struct stat st{};
    ::stat(file.c_str(), &st);
    expect((st.st_mode & 0777) == 0644, "a new file is 0644, not mkstemp's 0600");

    // Over an existing one: replaced, permissions kept.
    ::chmod(file.c_str(), 0640);
    expect(!config_codec::writeFileAtomically(file.string(), "b: 2\n"), "replaces an existing file");
    expect(slurp(file) == "b: 2\n", "the new contents replace the old");
    ::stat(file.c_str(), &st);
    expect((st.st_mode & 0777) == 0640, "the existing file's permissions are kept");
    expect(entriesIn(dir) == 1, "no temporary is left behind");

    // Into a directory that does not exist: refused, nothing created.
    const fs::path missing = dir / "nope" / "layout.yaml";
    expect(config_codec::writeFileAtomically(missing.string(), "c: 3\n").has_value(),
           "a missing directory is reported");

    // Over a target that is a directory: the rename fails after the write, so
    // the temporary existed and must be gone.
    const fs::path blocker = dir / "blocker";
    fs::create_directories(blocker / "inside");
    expect(config_codec::writeFileAtomically(blocker.string(), "d: 4\n").has_value(),
           "a rename that fails is reported");
    expect(entriesIn(dir) == 2, "and its temporary removed");
    expect(slurp(file) == "b: 2\n", "the untouched file is still whole");

    // The YAML wrapper ends the document with a newline.
    YAML::Node node;
    node["key"] = "value";
    expect(!config_codec::writeYamlFile(node, file.string()), "writes YAML");
    expect(slurp(file) == "key: value\n", "emitted and newline-terminated");

    fs::remove_all(dir);
    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
