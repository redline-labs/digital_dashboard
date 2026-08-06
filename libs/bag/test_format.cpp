// SPDX-License-Identifier: GPL-3.0-or-later
//
// That the files we write are structurally valid MCAP -- checked against the
// spec, not against our own reader.
//
// WHY THIS TEST EXISTS. Every other case in this library writes a bag and reads
// it back through BagReader, which sits on mcap's reader, which is lenient. That
// pair can agree with itself about a malformed file, and did:
//
//   Rolling a part emitted a summary listing schema and channel ids the data
//   section never contained. testRoundTrip, testSplitting and testSeeking all
//   passed. `bag info` reported correct counts. The only thing that noticed was
//   Foxglove's `mcap doctor`, which is a Go binary that is not installed, not in
//   CI, and not run by habit.
//
// bag::validateMcapFile walks the raw bytes against the published spec with no
// reference to mcap's code, so a writer bug and a reader bug cannot cancel out.
// These cases point it at files with known properties -- including files we
// damage on purpose -- and assert it reaches the right verdict.
//
// A validator is only worth having if it FAILS on bad input, so most of what
// follows is deliberately broken files.

#include "bag/validate.h"
#include "bag/writer.h"

#include <spdlog/spdlog.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
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
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

class TempDir
{
  public:
    explicit TempDir(const std::string& label)
    {
        path_ = std::filesystem::temp_directory_path() /
                ("redline_bag_format_" + label + "_" + std::to_string(::getpid()));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    std::string str() const { return path_.string(); }
    std::filesystem::path path() const { return path_; }

  private:
    std::filesystem::path path_;
};

constexpr std::uint64_t kBase = 1'785'000'000'000'000'000ull;

std::vector<std::uint8_t> payloadFor(int index, std::size_t size)
{
    std::vector<std::uint8_t> payload(size);
    for (std::size_t i = 0; i < size; ++i)
    {
        payload[i] = static_cast<std::uint8_t>((index * 31 + static_cast<int>(i) * 7) & 0xFF);
    }
    return payload;
}

// Writes a recording and returns the directory. `codec` is passed straight
// through so the same body covers all three.
void writeBag(const TempDir& dir, const std::string& codec, int count,
              std::uint64_t max_part_bytes = 0)
{
    bag::WriterOptions options;
    options.name = "f";
    options.compression = codec;
    options.chunk_bytes = 4 * 1024;
    options.max_part_bytes = max_part_bytes;

    bag::BagWriter writer(dir.str(), options);
    for (int i = 0; i < count; ++i)
    {
        const char* keys[] = {"a/one", "a/two", "a/three"};
        const char* schemas[] = {"EngineRpm", "VehicleSpeed", "EngineTemperature"};
        writer.write(keys[i % 3], schemas[i % 3], payloadFor(i, 256),
                     kBase + static_cast<std::uint64_t>(i) * 1'000'000ull,
                     kBase + static_cast<std::uint64_t>(i) * 1'000'000ull - 1000ull, "zid");
    }
    writer.close();
}

void reportFindings(const bag::ValidationReport& report, const std::string& label)
{
    for (const bag::Finding& finding : report.findings)
    {
        std::fprintf(stderr, "  [%s] %s: %s\n",
                     finding.severity == bag::Finding::Severity::Error ? "error" : "warn",
                     label.c_str(), finding.message.c_str());
    }
}

// ------------------------------------------------- what we write is valid MCAP

// The headline claim, across every codec we support. lz4 in particular had no
// coverage at all before this -- an entire third_party dependency that could
// have been compiled out via MCAP_COMPRESSION_NO_LZ4 without anything noticing.
void testEveryCodecProducesValidMcap()
{
    for (const std::string codec : {"none", "lz4", "zstd"})
    {
        const TempDir dir("codec_" + codec);
        writeBag(dir, codec, 300);

        const bag::ValidationReport report =
            bag::validateMcapFile((dir.path() / "f_0000.mcap").string());

        if (!report.ok())
        {
            reportFindings(report, codec);
        }

        expect(report.ok(), "a bag written with compression '" + codec + "' is valid MCAP");
        expect(report.messages == 300,
               "the validator counts every message with '" + codec + "' (" +
                   std::to_string(report.messages) + ")");
        expect(report.channels == 3, "and every channel with '" + codec + "'");
        expect(report.has_summary, "and the file has a summary with '" + codec + "'");

        // Proves the codec was actually applied rather than silently falling
        // back -- which is what a missing MCAP_COMPRESSION_NO_LZ4 guard would
        // look like from the outside.
        const std::string expected = codec == "none" ? "none" : codec;
        expect(report.compression == expected,
               "the chunks really are '" + codec + "' (validator saw '" + report.compression +
                   "')");
    }
}

// Rolled parts, validated individually. This is the regression from the bug
// `mcap doctor` found, now checked without `mcap doctor`.
void testRolledPartsAreValid()
{
    const TempDir dir("rolled");
    writeBag(dir, "none", 400, /*max_part_bytes=*/16 * 1024);

    std::size_t parts = 0;
    bool all_valid = true;

    for (const auto& entry : std::filesystem::directory_iterator(dir.path()))
    {
        if (entry.path().extension() != ".mcap")
        {
            continue;
        }
        ++parts;

        const bag::ValidationReport report = bag::validateMcapFile(entry.path().string());
        if (!report.ok())
        {
            reportFindings(report, entry.path().filename().string());
            all_valid = false;
        }
    }

    expect(parts > 1, "the recording rolled (" + std::to_string(parts) + " parts)");
    expect(all_valid, "every rolled part is valid MCAP on its own");
}

// The whole-directory check, including the index agreeing with the files.
void testWholeBagValidates()
{
    const TempDir dir("whole");
    writeBag(dir, "zstd", 400, /*max_part_bytes=*/16 * 1024);

    const bag::ValidationReport report = bag::validateBag(dir.str());
    if (!report.ok())
    {
        reportFindings(report, "bag");
    }
    expect(report.ok(), "a whole bag directory validates");
    expect(report.messages == 400,
           "and the parts together hold every message (" + std::to_string(report.messages) + ")");
}

// ------------------------------------------------ the validator catches damage
//
// A validator that never fails is decoration. Each of these breaks a file in a
// specific way and asserts the specific complaint.

std::vector<std::uint8_t> readFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
}

void writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

bool mentions(const bag::ValidationReport& report, const std::string& fragment)
{
    for (const bag::Finding& finding : report.findings)
    {
        if (finding.severity == bag::Finding::Severity::Error &&
            finding.message.find(fragment) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

void testTruncationIsCaught()
{
    const TempDir dir("truncated");
    writeBag(dir, "zstd", 300);

    const auto part = dir.path() / "f_0000.mcap";
    std::filesystem::resize_file(part, std::filesystem::file_size(part) / 2);

    const bag::ValidationReport report = bag::validateMcapFile(part.string());
    expect(!report.ok(), "a truncated file does not validate");
    expect(mentions(report, "magic"),
           "and the complaint names the missing trailing magic, which is what a killed writer "
           "leaves behind");
}

void testCorruptedMagicIsCaught()
{
    const TempDir dir("magic");
    writeBag(dir, "none", 10);

    const auto part = dir.path() / "f_0000.mcap";
    std::vector<std::uint8_t> bytes = readFile(part);
    bytes[1] = 'X';  // 0x89 'M' -> 0x89 'X'
    writeFile(part, bytes);

    const bag::ValidationReport report = bag::validateMcapFile(part.string());
    expect(!report.ok(), "a file with the wrong leading magic does not validate");
}

// Offset of the first byte of the first Chunk record's `records` field.
//
// The test needs this precisely rather than picking a byte at random. An
// arbitrary offset in the middle of the file lands in a MessageIndex record as
// often as not -- those sit between chunks and are NOT covered by any CRC, so
// corrupting one is invisible by design and asserting otherwise would be
// testing a guarantee the format does not make.
//
// Walks the same spec framing the validator does: <op><uint64 length><content>.
std::size_t firstChunkPayloadOffset(const std::vector<std::uint8_t>& bytes)
{
    const auto readU64 = [&bytes](std::size_t at)
    {
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i)
        {
            value |= static_cast<std::uint64_t>(bytes[at + static_cast<std::size_t>(i)])
                     << (8u * static_cast<unsigned>(i));
        }
        return value;
    };
    const auto readU32 = [&bytes](std::size_t at)
    {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i)
        {
            value |= static_cast<std::uint32_t>(bytes[at + static_cast<std::size_t>(i)])
                     << (8u * static_cast<unsigned>(i));
        }
        return value;
    };

    std::size_t offset = 8;  // past the leading magic
    while (offset + 9 < bytes.size())
    {
        const std::uint8_t op = bytes[offset];
        const std::uint64_t length = readU64(offset + 1);

        if (op == 0x06)  // Chunk
        {
            // start(8) end(8) uncompressed_size(8) uncompressed_crc(4)
            // compression(uint32 len + bytes) records(uint64 len + bytes)
            std::size_t cursor = offset + 9 + 8 + 8 + 8 + 4;
            const std::uint32_t codec_length = readU32(cursor);
            cursor += 4 + codec_length;
            cursor += 8;  // the records length prefix
            return cursor;
        }

        offset += 9 + static_cast<std::size_t>(length);
    }
    return 0;
}

// A flipped byte inside a chunk's payload. The records still tile perfectly and
// every length is intact, so nothing structural is wrong -- only the CRC knows.
// This is the case a purely structural parser would miss entirely, which is why
// the validator computes CRC32 itself.
void testChunkCrcMismatchIsCaught()
{
    const TempDir dir("crc");
    writeBag(dir, "none", 200);

    const auto part = dir.path() / "f_0000.mcap";
    std::vector<std::uint8_t> bytes = readFile(part);

    const std::size_t target = firstChunkPayloadOffset(bytes);
    expect(target > 0 && target < bytes.size(), "the file has a chunk to corrupt");
    if (target == 0 || target >= bytes.size())
    {
        return;
    }

    // Well inside the payload, so it cannot be mistaken for a framing byte.
    const std::size_t victim = target + 64;
    bytes[victim] = static_cast<std::uint8_t>(bytes[victim] ^ 0xFFu);
    writeFile(part, bytes);

    const bag::ValidationReport report = bag::validateMcapFile(part.string());
    expect(!report.ok(),
           "a bit flipped inside a chunk is caught -- the records still tile perfectly, so only "
           "the CRC can tell");
    expect(mentions(report, "CRC"), "and the complaint says so");
}

// A part on disk the index does not list. `bag reindex` would pick it up; until
// then it is data no reader will ever return.
void testUnlistedPartIsCaught()
{
    const TempDir dir("unlisted");
    writeBag(dir, "none", 50);

    std::filesystem::copy_file(dir.path() / "f_0000.mcap", dir.path() / "stray_0000.mcap");

    const bag::ValidationReport report = bag::validateBag(dir.str());
    expect(!report.ok(), "a part on disk that metadata.yaml does not list is an error");
    expect(mentions(report, "stray_0000.mcap"), "and it is named");
}

// The index and the file disagreeing about how many messages there are. They
// are written at different moments, so a crash between them produces exactly
// this.
void testIndexDisagreementIsCaught()
{
    const TempDir dir("disagree");
    writeBag(dir, "none", 100);

    // Rewrite metadata.yaml with a wrong count.
    auto metadata = bag::loadMetadata(dir.str());
    expect(metadata.has_value(), "the index loads");
    if (!metadata)
    {
        return;
    }
    metadata->parts.front().message_count = 999;
    metadata->message_count = 999;
    bag::saveMetadata(*metadata, dir.str());

    const bag::ValidationReport report = bag::validateBag(dir.str());
    expect(!report.ok(), "an index that disagrees with its files is an error");
    expect(mentions(report, "999"), "and the complaint quotes both numbers");
}

// A bag with nothing in it. Legal, and a real case -- `bag record` stopped
// before anything published.
void testEmptyRecordingIsValid()
{
    const TempDir dir("empty");

    {
        bag::WriterOptions options;
        options.name = "f";
        bag::BagWriter writer(dir.str(), options);
        expect(writer.isValid(), "a writer with no messages still opens");
        expect(writer.close(), "and closes cleanly");
    }

    const bag::ValidationReport report =
        bag::validateMcapFile((dir.path() / "f_0000.mcap").string());
    if (!report.ok())
    {
        reportFindings(report, "empty");
    }
    expect(report.ok(), "a recording with zero messages is still valid MCAP");
    expect(report.messages == 0, "and reports zero messages");

    const bag::ValidationReport bag_report = bag::validateBag(dir.str());
    expect(bag_report.ok(), "and the directory validates too");
}

// The validator must not crash, hang, or read out of bounds on arbitrary bytes.
// It is pointed at damaged files by definition, so this is a requirement rather
// than politeness.
void testGarbageDoesNotBreakTheValidator()
{
    const TempDir dir("garbage");

    {
        std::vector<std::uint8_t> junk(4096);
        for (std::size_t i = 0; i < junk.size(); ++i)
        {
            junk[i] = static_cast<std::uint8_t>((i * 37) & 0xFF);
        }
        writeFile(dir.path() / "junk.mcap", junk);
    }

    const bag::ValidationReport report =
        bag::validateMcapFile((dir.path() / "junk.mcap").string());
    expect(!report.ok(), "random bytes are not valid MCAP");

    // A file that IS magic-prefixed but garbage afterwards, which gets past the
    // first check and into the record walk with nonsense lengths.
    {
        std::vector<std::uint8_t> bytes{0x89, 'M', 'C', 'A', 'P', 0x30, '\r', '\n'};
        bytes.push_back(0x05);  // a Message opcode
        for (int i = 0; i < 8; ++i)
        {
            bytes.push_back(0xFF);  // a length of ~1.8e19
        }
        writeFile(dir.path() / "lying.mcap", bytes);
    }

    const bag::ValidationReport lying =
        bag::validateMcapFile((dir.path() / "lying.mcap").string());
    expect(!lying.ok(), "a record claiming an absurd length is rejected rather than trusted");

    // Empty and tiny files.
    writeFile(dir.path() / "tiny.mcap", {0x89, 'M'});
    expect(!bag::validateMcapFile((dir.path() / "tiny.mcap").string()).ok(),
           "a 2-byte file is rejected");

    writeFile(dir.path() / "nothing.mcap", {});
    expect(!bag::validateMcapFile((dir.path() / "nothing.mcap").string()).ok(),
           "an empty file is rejected");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);

    testEveryCodecProducesValidMcap();
    testRolledPartsAreValid();
    testWholeBagValidates();
    testTruncationIsCaught();
    testCorruptedMagicIsCaught();
    testChunkCrcMismatchIsCaught();
    testUnlistedPartIsCaught();
    testIndexDisagreementIsCaught();
    testEmptyRecordingIsValid();
    testGarbageDoesNotBreakTheValidator();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
