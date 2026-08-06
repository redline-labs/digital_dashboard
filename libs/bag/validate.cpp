#include "bag/validate.h"

#include "bag/metadata.h"

#include <lz4frame.h>
#include <zstd.h>

#include <spdlog/spdlog.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <system_error>

namespace bag
{

namespace
{

// ------------------------------------------------------------------ the spec
//
// From build/_deps/mcap-src/website/docs/spec/index.md. Written out here rather
// than included from mcap so that a parser bug in mcap and a writer bug in us
// cannot agree with each other -- which is the entire point of this file.

constexpr std::array<std::uint8_t, 8> kMagic{0x89, 'M', 'C', 'A', 'P', 0x30, '\r', '\n'};

enum Op : std::uint8_t
{
    kHeader = 0x01,
    kFooter = 0x02,
    kSchema = 0x03,
    kChannel = 0x04,
    kMessage = 0x05,
    kChunk = 0x06,
    kMessageIndex = 0x07,
    kChunkIndex = 0x08,
    kAttachment = 0x09,
    kAttachmentIndex = 0x0A,
    kStatistics = 0x0B,
    kMetadata = 0x0C,
    kMetadataIndex = 0x0D,
    kSummaryOffset = 0x0E,
    kDataEnd = 0x0F,
};

// CRC32, IEEE 802.3 reflected -- polynomial 0xEDB88320, init 0xFFFFFFFF, final
// complement. The same one the spec names and zlib implements.
std::uint32_t crc32(const std::uint8_t* data, std::size_t length)
{
    static const std::array<std::uint32_t, 256> table = []
    {
        std::array<std::uint32_t, 256> out{};
        for (std::uint32_t i = 0; i < 256; ++i)
        {
            std::uint32_t r = i;
            for (int bit = 0; bit < 8; ++bit)
            {
                r = ((r & 1u) != 0u) ? (0xEDB88320u ^ (r >> 1u)) : (r >> 1u);
            }
            out[i] = r;
        }
        return out;
    }();

    std::uint32_t r = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < length; ++i)
    {
        r = table[(r ^ data[i]) & 0xFFu] ^ (r >> 8u);
    }
    return ~r;
}

// A bounds-checked cursor over a byte range. Every read that would run past the
// end sets `overrun` instead of reading -- a validator that segfaulted on a
// malformed file would be worse than no validator, since malformed files are
// precisely what it is pointed at.
class Cursor
{
  public:
    Cursor(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool overrun() const { return overrun_; }
    std::size_t remaining() const { return overrun_ ? 0 : size_ - offset_; }

    std::uint8_t u8()
    {
        if (!check(1))
        {
            return 0;
        }
        return data_[offset_++];
    }

    std::uint16_t u16()
    {
        if (!check(2))
        {
            return 0;
        }
        const std::uint16_t value = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(data_[offset_]) |
            (static_cast<std::uint16_t>(data_[offset_ + 1]) << 8u));
        offset_ += 2;
        return value;
    }

    std::uint32_t u32()
    {
        if (!check(4))
        {
            return 0;
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i)
        {
            value |= static_cast<std::uint32_t>(data_[offset_ + static_cast<std::size_t>(i)])
                     << (8u * static_cast<unsigned>(i));
        }
        offset_ += 4;
        return value;
    }

    std::uint64_t u64()
    {
        if (!check(8))
        {
            return 0;
        }
        std::uint64_t value = 0;
        for (int i = 0; i < 8; ++i)
        {
            value |= static_cast<std::uint64_t>(data_[offset_ + static_cast<std::size_t>(i)])
                     << (8u * static_cast<unsigned>(i));
        }
        offset_ += 8;
        return value;
    }

    // uint32 length prefix, then the bytes.
    std::string str()
    {
        const std::uint32_t length = u32();
        if (!check(length))
        {
            return {};
        }
        std::string value(reinterpret_cast<const char*>(data_ + offset_), length);
        offset_ += length;
        return value;
    }

    // Skips `count` bytes.
    void skip(std::size_t count)
    {
        if (check(count))
        {
            offset_ += count;
        }
    }

    const std::uint8_t* here() const { return data_ + offset_; }

  private:
    bool check(std::size_t needed)
    {
        if (overrun_ || offset_ + needed > size_)
        {
            overrun_ = true;
            return false;
        }
        return true;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_ = 0;
    bool overrun_ = false;
};

// Everything the walk accumulates, split by section -- because the summary
// repeating a record the data section does not contain is the exact bug this
// was written for.
struct Observed
{
    std::set<std::uint16_t> data_schema_ids;
    std::set<std::uint16_t> data_channel_ids;
    std::map<std::uint16_t, std::uint16_t> data_channel_schema;
    std::map<std::uint16_t, std::string> data_channel_topic;

    std::set<std::uint16_t> summary_schema_ids;
    std::set<std::uint16_t> summary_channel_ids;
    std::map<std::uint16_t, std::uint16_t> summary_channel_schema;

    std::set<std::uint16_t> referenced_channel_ids;  // by Message records

    std::uint64_t messages = 0;
    std::uint32_t chunks = 0;

    bool saw_header = false;
    bool saw_data_end = false;
    bool saw_footer = false;
    bool saw_statistics = false;

    std::uint64_t statistics_messages = 0;
    std::uint32_t statistics_chunks = 0;

    std::string compression;

    // Byte offsets, from the start of the file, of every record of a kind an
    // index is allowed to point at -- and how long each one is.
    //
    // The indexes are what make seeking work, and they are pure redundancy: an
    // offset that is wrong does not corrupt any message, it just sends a reader
    // somewhere else. Nothing detects that except comparing the two, which is
    // what these exist for.
    std::map<std::uint64_t, std::uint64_t> chunk_offsets;          // offset -> total length
    std::map<std::uint64_t, std::uint16_t> message_index_offsets;  // offset -> channel id

    // Whatever the ChunkIndex records claim, checked once the walk has seen the
    // whole file -- the summary comes after the data, so the claims arrive
    // after the things they point at.
    struct ChunkClaim
    {
        std::uint64_t chunk_start_offset = 0;
        std::uint64_t chunk_length = 0;
        std::uint64_t message_index_length = 0;
        std::map<std::uint16_t, std::uint64_t> message_index_offsets;
    };
    std::vector<ChunkClaim> chunk_claims;

    // SummaryOffset groups: opcode -> (start, length).
    struct GroupClaim
    {
        std::uint8_t opcode = 0;
        std::uint64_t start = 0;
        std::uint64_t length = 0;
    };
    std::vector<GroupClaim> group_claims;

    std::uint64_t data_end_offset = 0;
    std::uint64_t footer_offset = 0;
};

std::vector<std::uint8_t> decompress(const std::string& codec,
                                     const std::uint8_t* data,
                                     std::size_t compressed_size,
                                     std::uint64_t uncompressed_size,
                                     std::string& error)
{
    if (codec.empty())
    {
        return std::vector<std::uint8_t>(data, data + compressed_size);
    }

    std::vector<std::uint8_t> out(uncompressed_size);

    if (codec == "zstd")
    {
        const std::size_t produced =
            ZSTD_decompress(out.data(), out.size(), data, compressed_size);
        if (ZSTD_isError(produced))
        {
            error = std::string("zstd: ") + ZSTD_getErrorName(produced);
            return {};
        }
        out.resize(produced);
        return out;
    }

    if (codec == "lz4")
    {
        // FRAME format, which is what mcap writes (LZ4F_compressFrame) -- the
        // block API would silently fail to parse the frame header.
        LZ4F_dctx* context = nullptr;
        if (LZ4F_isError(LZ4F_createDecompressionContext(&context, LZ4F_VERSION)))
        {
            error = "lz4: could not create a decompression context";
            return {};
        }

        std::size_t destination_size = out.size();
        std::size_t source_size = compressed_size;
        const std::size_t status =
            LZ4F_decompress(context, out.data(), &destination_size, data, &source_size, nullptr);
        LZ4F_freeDecompressionContext(context);

        if (LZ4F_isError(status))
        {
            error = std::string("lz4: ") + LZ4F_getErrorName(status);
            return {};
        }
        out.resize(destination_size);
        return out;
    }

    error = "unknown compression '" + codec + "'";
    return {};
}

class Validator
{
  public:
    explicit Validator(ValidationReport& report) : report_(report) {}

    void error(const std::string& message)
    {
        report_.findings.push_back({Finding::Severity::Error, message});
    }

    void warn(const std::string& message)
    {
        report_.findings.push_back({Finding::Severity::Warning, message});
    }

    // Walks a record stream. `in_summary` selects which side of the
    // data/summary divide the records land on.
    // `base` is the file offset of `data[0]`, so records inside the top-level
    // walk can record where they actually live. Records inside a chunk get a
    // base of zero -- their offsets are relative to the decompressed stream and
    // are not file offsets at all.
    void walkRecords(const std::uint8_t* data, std::size_t size, bool inside_chunk,
                     std::uint64_t base = 0)
    {
        std::size_t offset = 0;
        while (offset < size)
        {
            if (offset + 9 > size)
            {
                error("a record header runs past the end of its enclosing " +
                      std::string(inside_chunk ? "chunk" : "file"));
                return;
            }

            const std::uint8_t op = data[offset];
            Cursor length_cursor(data + offset + 1, 8);
            const std::uint64_t length = length_cursor.u64();

            if (offset + 9 + length > size)
            {
                error("record 0x" + toHex(op) + " claims " + std::to_string(length) +
                      " bytes but only " + std::to_string(size - offset - 9) + " remain");
                return;
            }

            handleRecord(op, data + offset + 9, length, inside_chunk,
                         base + static_cast<std::uint64_t>(offset), 9 + length);
            offset += 9 + static_cast<std::size_t>(length);
        }
    }

    Observed& observed() { return observed_; }

  private:
    static std::string toHex(std::uint8_t value)
    {
        static const char* digits = "0123456789ABCDEF";
        return std::string(1, digits[value >> 4u]) + std::string(1, digits[value & 0x0Fu]);
    }

    void handleRecord(std::uint8_t op, const std::uint8_t* content, std::uint64_t length,
                      bool inside_chunk, std::uint64_t record_offset,
                      std::uint64_t record_total_length)
    {
        Cursor cursor(content, static_cast<std::size_t>(length));

        switch (op)
        {
            case kHeader:
                observed_.saw_header = true;
                break;

            case kDataEnd:
                observed_.saw_data_end = true;
                observed_.data_end_offset = record_offset;
                break;

            case kFooter:
                observed_.saw_footer = true;
                observed_.footer_offset = record_offset;
                break;

            case kSchema:
            {
                const std::uint16_t id = cursor.u16();
                (void)cursor.str();  // name
                (void)cursor.str();  // encoding
                const std::uint32_t data_length = cursor.u32();
                cursor.skip(data_length);

                if (id == 0)
                {
                    error("a Schema record has id 0, which the spec reserves for 'no schema'");
                }

                // A schema inside a chunk still belongs to the data section.
                if (observed_.saw_data_end && !inside_chunk)
                {
                    observed_.summary_schema_ids.insert(id);
                }
                else
                {
                    observed_.data_schema_ids.insert(id);
                }
                break;
            }

            case kChannel:
            {
                const std::uint16_t id = cursor.u16();
                const std::uint16_t schema_id = cursor.u16();
                const std::string topic = cursor.str();

                if (observed_.saw_data_end && !inside_chunk)
                {
                    observed_.summary_channel_ids.insert(id);
                    observed_.summary_channel_schema[id] = schema_id;
                }
                else
                {
                    observed_.data_channel_ids.insert(id);
                    observed_.data_channel_schema[id] = schema_id;
                    observed_.data_channel_topic[id] = topic;
                }
                break;
            }

            case kMessage:
            {
                const std::uint16_t channel_id = cursor.u16();
                observed_.referenced_channel_ids.insert(channel_id);
                ++observed_.messages;
                break;
            }

            case kChunk:
            {
                ++observed_.chunks;

                // Where this chunk actually is, so a ChunkIndex claiming to
                // point at one can be checked against reality.
                if (!inside_chunk)
                {
                    observed_.chunk_offsets[record_offset] = record_total_length;
                }

                (void)cursor.u64();  // message_start_time
                (void)cursor.u64();  // message_end_time
                const std::uint64_t uncompressed_size = cursor.u64();
                const std::uint32_t uncompressed_crc = cursor.u32();
                const std::string codec = cursor.str();
                const std::uint64_t records_length = cursor.u64();

                if (cursor.overrun() || records_length > cursor.remaining())
                {
                    error("a Chunk record's fields run past its own length");
                    return;
                }

                if (observed_.compression.empty())
                {
                    observed_.compression = codec.empty() ? "none" : codec;
                }

                std::string problem;
                const std::vector<std::uint8_t> records =
                    decompress(codec, cursor.here(), static_cast<std::size_t>(records_length),
                               uncompressed_size, problem);

                if (!problem.empty())
                {
                    error("a Chunk record could not be decompressed: " + problem);
                    return;
                }

                if (records.size() != uncompressed_size)
                {
                    error("a Chunk record declares uncompressed_size " +
                          std::to_string(uncompressed_size) + " but produced " +
                          std::to_string(records.size()) + " bytes");
                }

                // An independent check that the bytes are what the writer
                // thought they were. A CRC mismatch means the chunk is corrupt
                // in a way no amount of structural parsing would otherwise
                // reveal -- every record inside it would still tile perfectly.
                if (uncompressed_crc != 0 && !records.empty())
                {
                    const std::uint32_t actual = crc32(records.data(), records.size());
                    if (actual != uncompressed_crc)
                    {
                        error("a Chunk record's uncompressed CRC32 does not match its contents");
                    }
                }

                // Records inside a chunk are data-section records wherever the
                // chunk itself sits.
                walkRecords(records.data(), records.size(), /*inside_chunk=*/true);
                break;
            }

            case kStatistics:
            {
                observed_.saw_statistics = true;
                observed_.statistics_messages = cursor.u64();
                (void)cursor.u16();  // schema_count
                (void)cursor.u32();  // channel_count
                (void)cursor.u32();  // attachment_count
                (void)cursor.u32();  // metadata_count
                observed_.statistics_chunks = cursor.u32();
                break;
            }

            case kMessageIndex:
            {
                const std::uint16_t channel_id = cursor.u16();
                const std::uint32_t records_length = cursor.u32();

                // Array<Tuple<Timestamp, uint64>> -- 16 bytes per entry, so a
                // length that is not a multiple of 16 cannot be what it claims.
                if (records_length % 16u != 0u)
                {
                    error("a MessageIndex record's array is " +
                          std::to_string(records_length) +
                          " bytes, which is not a whole number of 16-byte entries");
                }
                cursor.skip(records_length);

                if (!inside_chunk)
                {
                    observed_.message_index_offsets[record_offset] = channel_id;
                }
                break;
            }

            case kChunkIndex:
            {
                Observed::ChunkClaim claim;
                (void)cursor.u64();  // message_start_time
                (void)cursor.u64();  // message_end_time
                claim.chunk_start_offset = cursor.u64();
                claim.chunk_length = cursor.u64();

                // Map<uint16, uint64>: a uint32 byte length, then 10-byte pairs.
                const std::uint32_t map_length = cursor.u32();
                if (map_length % 10u != 0u)
                {
                    error("a ChunkIndex record's message_index_offsets map is " +
                          std::to_string(map_length) +
                          " bytes, which is not a whole number of 10-byte entries");
                    cursor.skip(map_length);
                }
                else
                {
                    for (std::uint32_t consumed = 0; consumed < map_length; consumed += 10u)
                    {
                        const std::uint16_t channel_id = cursor.u16();
                        claim.message_index_offsets[channel_id] = cursor.u64();
                    }
                }

                claim.message_index_length = cursor.u64();
                (void)cursor.str();  // compression
                (void)cursor.u64();  // compressed_size
                (void)cursor.u64();  // uncompressed_size

                if (!cursor.overrun())
                {
                    observed_.chunk_claims.push_back(std::move(claim));
                }
                break;
            }

            case kSummaryOffset:
            {
                Observed::GroupClaim claim;
                claim.opcode = cursor.u8();
                claim.start = cursor.u64();
                claim.length = cursor.u64();
                if (!cursor.overrun())
                {
                    observed_.group_claims.push_back(claim);
                }
                break;
            }

            case kAttachment:
            case kAttachmentIndex:
            case kMetadata:
            case kMetadataIndex:
                break;

            default:
                // 0x00 is never valid; 0x80-0xFF are private and legal to
                // ignore; anything else in 0x01-0x7F is reserved for a future
                // MCAP version we do not know.
                if (op == 0x00)
                {
                    error("record opcode 0x00 is not valid");
                }
                else if (op < 0x80)
                {
                    warn("unknown reserved record opcode 0x" + toHex(op) +
                         " -- written by a newer MCAP version?");
                }
                break;
        }

        if (cursor.overrun())
        {
            error("record 0x" + toHex(op) + " is shorter than its own fields require");
        }
    }

    ValidationReport& report_;
    Observed observed_;
};

}  // namespace

bool hasCompleteEnding(const std::string& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        return false;
    }

    const std::streamoff size = file.tellg();
    if (size < static_cast<std::streamoff>(kMagic.size()))
    {
        return false;
    }

    file.seekg(size - static_cast<std::streamoff>(kMagic.size()));

    std::array<std::uint8_t, kMagic.size()> tail{};
    file.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail.size()));
    if (!file)
    {
        return false;
    }

    return std::equal(kMagic.begin(), kMagic.end(), tail.begin());
}

ValidationReport validateMcapFile(const std::string& path)
{
    ValidationReport report;
    Validator validator(report);

    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        validator.error("could not open '" + path + "'");
        return report;
    }

    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());

    if (bytes.size() < kMagic.size() * 2)
    {
        validator.error("file is too short to be an MCAP -- " + std::to_string(bytes.size()) +
                        " bytes, and the magic alone is " + std::to_string(kMagic.size() * 2));
        return report;
    }

    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
    {
        validator.error("the file does not start with the MCAP magic bytes");
        return report;
    }

    const bool trailing_magic_ok =
        std::equal(kMagic.begin(), kMagic.end(), bytes.end() - static_cast<long>(kMagic.size()));
    if (!trailing_magic_ok)
    {
        // The signature of a writer that was killed: everything before this may
        // be perfectly good, so the walk continues.
        validator.error("the file does not end with the MCAP magic bytes -- it was truncated, or "
                        "its writer never closed it");
    }

    // Records tile the span between the two magics exactly. If they do not, the
    // walk reports where it ran out.
    const std::size_t body_end =
        trailing_magic_ok ? bytes.size() - kMagic.size() : bytes.size();
    validator.walkRecords(bytes.data() + kMagic.size(), body_end - kMagic.size(),
                          /*inside_chunk=*/false, /*base=*/kMagic.size());

    const Observed& observed = validator.observed();

    report.messages = observed.messages;
    report.chunks = observed.chunks;
    report.schemas = observed.data_schema_ids.size();
    report.channels = observed.data_channel_ids.size();
    report.compression = observed.compression.empty() ? "none" : observed.compression;
    report.has_summary = observed.saw_data_end;

    if (!observed.saw_header)
    {
        validator.error("no Header record");
    }
    if (trailing_magic_ok && !observed.saw_footer)
    {
        validator.error("no Footer record");
    }
    if (trailing_magic_ok && !observed.saw_data_end)
    {
        validator.error("no DataEnd record -- the data section was never closed");
    }

    // THE CHECK THIS FILE WAS WRITTEN FOR.
    //
    // The summary repeats the data section's Schema and Channel records so a
    // reader can build its indexes without scanning. Repeating one that is NOT
    // in the data section makes the file self-contradictory: an index entry
    // pointing at a record that does not exist. Our own reader did not care;
    // another implementation is entitled to reject the file.
    for (const std::uint16_t id : observed.summary_channel_ids)
    {
        if (observed.data_channel_ids.count(id) == 0)
        {
            validator.error("Channel id " + std::to_string(id) +
                            " appears in the summary section but not in the data section");
        }
    }
    for (const std::uint16_t id : observed.summary_schema_ids)
    {
        if (observed.data_schema_ids.count(id) == 0)
        {
            validator.error("Schema id " + std::to_string(id) +
                            " appears in the summary section but not in the data section");
        }
    }

    // Every channel must name a schema that exists (0 means "no schema", which
    // is legal).
    for (const auto& [channel_id, schema_id] : observed.data_channel_schema)
    {
        if (schema_id != 0 && observed.data_schema_ids.count(schema_id) == 0)
        {
            validator.error("Channel " + std::to_string(channel_id) + " references Schema " +
                            std::to_string(schema_id) + ", which the file does not contain");
        }
    }

    // Every message must name a channel that exists.
    for (const std::uint16_t channel_id : observed.referenced_channel_ids)
    {
        if (observed.data_channel_ids.count(channel_id) == 0)
        {
            validator.error("a Message references Channel " + std::to_string(channel_id) +
                            ", which the file does not contain");
        }
    }

    // ---------------------------------------------------------- the indexes
    //
    // These are what make `bag play --start-offset` a seek and `bag info`
    // instant. They are also PURE REDUNDANCY: an offset that points at the
    // wrong place corrupts no message and fails no structural check -- it just
    // sends a reader somewhere else. Comparing the claims against where the
    // records actually turned out to be is the only thing that detects it.
    for (const Observed::ChunkClaim& claim : observed.chunk_claims)
    {
        const auto found = observed.chunk_offsets.find(claim.chunk_start_offset);
        if (found == observed.chunk_offsets.end())
        {
            validator.error("a ChunkIndex points at offset " +
                            std::to_string(claim.chunk_start_offset) +
                            ", where there is no Chunk record");
            continue;
        }

        if (claim.chunk_length != found->second)
        {
            validator.error("a ChunkIndex says the chunk at offset " +
                            std::to_string(claim.chunk_start_offset) + " is " +
                            std::to_string(claim.chunk_length) + " bytes; it is " +
                            std::to_string(found->second));
        }

        for (const auto& [channel_id, offset] : claim.message_index_offsets)
        {
            const auto index = observed.message_index_offsets.find(offset);
            if (index == observed.message_index_offsets.end())
            {
                validator.error("a ChunkIndex points channel " + std::to_string(channel_id) +
                                " at offset " + std::to_string(offset) +
                                ", where there is no MessageIndex record");
            }
            else if (index->second != channel_id)
            {
                validator.error("a ChunkIndex points channel " + std::to_string(channel_id) +
                                " at a MessageIndex for channel " +
                                std::to_string(index->second));
            }
        }
    }

    // Every chunk should be indexed, or seeking silently degrades to a scan
    // over the part of the file nothing points into.
    if (!observed.chunk_claims.empty() &&
        observed.chunk_claims.size() != observed.chunk_offsets.size())
    {
        validator.error("the file has " + std::to_string(observed.chunk_offsets.size()) +
                        " Chunk records but " + std::to_string(observed.chunk_claims.size()) +
                        " ChunkIndex records -- some chunks are unreachable by a seek");
    }

    // SummaryOffset groups have to land inside the summary section, between
    // DataEnd and the Footer. A group pointing outside it sends a reader that
    // trusts it into the message data.
    for (const Observed::GroupClaim& claim : observed.group_claims)
    {
        if (observed.data_end_offset == 0 || observed.footer_offset == 0)
        {
            break;
        }
        if (claim.start < observed.data_end_offset ||
            claim.start + claim.length > observed.footer_offset)
        {
            validator.error("a SummaryOffset for opcode 0x" +
                            std::string(1, "0123456789ABCDEF"[claim.opcode >> 4u]) +
                            std::string(1, "0123456789ABCDEF"[claim.opcode & 0x0Fu]) +
                            " spans [" + std::to_string(claim.start) + ", " +
                            std::to_string(claim.start + claim.length) +
                            "), which is outside the summary section");
        }
    }

    // Statistics is what a reader trusts instead of counting, so it disagreeing
    // with the actual contents is worse than it being absent.
    if (observed.saw_statistics)
    {
        if (observed.statistics_messages != observed.messages)
        {
            validator.error("Statistics claims " + std::to_string(observed.statistics_messages) +
                            " messages but the file contains " +
                            std::to_string(observed.messages));
        }
        if (observed.statistics_chunks != observed.chunks)
        {
            validator.error("Statistics claims " + std::to_string(observed.statistics_chunks) +
                            " chunks but the file contains " + std::to_string(observed.chunks));
        }
    }
    else if (trailing_magic_ok)
    {
        validator.warn("no Statistics record -- `bag info` would have to scan");
    }

    return report;
}

ValidationReport validateBag(const std::string& directory)
{
    ValidationReport report;
    Validator validator(report);

    const auto metadata = loadMetadata(directory);
    if (!metadata)
    {
        validator.error("no readable metadata.yaml in '" + directory + "'");
        return report;
    }

    std::set<std::string> listed;
    std::uint64_t counted_messages = 0;

    for (const bag_part_t& part : metadata->parts)
    {
        listed.insert(part.path);

        const std::filesystem::path path = std::filesystem::path(directory) / part.path;

        std::error_code error;
        if (!std::filesystem::exists(path, error))
        {
            validator.error("part '" + part.path + "' is in metadata.yaml but not on disk");
            continue;
        }

        ValidationReport part_report = validateMcapFile(path.string());
        for (Finding& finding : part_report.findings)
        {
            finding.message = part.path + ": " + finding.message;
            report.findings.push_back(std::move(finding));
        }

        report.messages += part_report.messages;
        report.chunks += part_report.chunks;
        if (report.compression.empty())
        {
            report.compression = part_report.compression;
        }

        // The index and the file have to agree. They are written at different
        // moments -- the part when it is closed, the index after -- so a crash
        // between them is exactly when they diverge.
        if (part_report.messages != part.message_count)
        {
            validator.error(part.path + ": metadata.yaml says " +
                            std::to_string(part.message_count) + " messages, the file contains " +
                            std::to_string(part_report.messages));
        }

        counted_messages += part_report.messages;
    }

    if (counted_messages != metadata->message_count)
    {
        validator.error("metadata.yaml says " + std::to_string(metadata->message_count) +
                        " messages in total, the parts contain " +
                        std::to_string(counted_messages));
    }

    // A part on disk that the index does not know about is data that no reader
    // will ever return.
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".mcap" &&
            listed.count(entry.path().filename().string()) == 0)
        {
            validator.error("'" + entry.path().filename().string() +
                            "' is on disk but not in metadata.yaml -- `bag reindex` would "
                            "pick it up");
        }
    }

    return report;
}

}  // namespace bag
