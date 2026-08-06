#ifndef BAG_WRITER_H_
#define BAG_WRITER_H_

#include "bag/metadata.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace bag
{

struct WriterOptions
{
    // "none", "lz4" or "zstd". zstd by default: its ratio/speed curve is
    // tunable, and level 1 keeps up with a full bus while roughly halving the
    // file.
    std::string compression = "zstd";

    // 0 means the codec's default. Higher costs CPU on the recorder, which on
    // the embedded target is the scarce resource -- not disk.
    int compression_level = 0;

    // Uncompressed bytes buffered before a chunk is flushed. Bigger compresses
    // better; smaller bounds how much a torn tail costs, since a partial chunk
    // is unreadable in its entirety.
    std::uint64_t chunk_bytes = 4ull * 1024 * 1024;

    // Roll to a new part past this size. Zero disables size rolling.
    std::uint64_t max_part_bytes = 2ull * 1024 * 1024 * 1024;

    // Roll to a new part past this many seconds. Zero disables time rolling.
    double max_part_seconds = 0.0;

    // Base name for the parts; "<name>_0000.mcap". Defaults to the bag
    // directory's own name.
    std::string name;

    // Recorded in metadata.yaml so a file can say what made it.
    std::string recorder = "redline bag";
};

// Writes a bag directory: metadata.yaml plus rolled .mcap parts.
//
// NOT THREAD-SAFE, deliberately. A recorder's zenoh callbacks must not block
// (an exception or a stall crossing the Rust FFI boundary is fatal), so the
// intended shape is: callback copies bytes into a queue and returns; ONE writer
// thread drains that queue into this. Making the writer itself lockable would
// invite calling it straight from the callback, which is the thing that must not
// happen.
class BagWriter
{
  public:
    BagWriter(std::string directory, WriterOptions options);
    ~BagWriter();

    BagWriter(const BagWriter&) = delete;
    BagWriter& operator=(const BagWriter&) = delete;

    // False when the directory could not be created or the first part could not
    // be opened. Nothing else is worth attempting after that.
    bool isValid() const;

    // Record one message.
    //
    // `publish_time_ns` is absent for a sample that arrived unstamped, in which
    // case log_time is used for both and the count in metadata goes up. That
    // count is the honest part: a consumer computing latency from a bag needs to
    // know how much of its publish_time was invented.
    //
    // Returns false on a write error, after which the recording should stop --
    // continuing would produce a file whose index does not describe its
    // contents.
    bool write(std::string_view key, std::string_view schema_name,
               std::span<const std::uint8_t> payload, std::uint64_t log_time_ns,
               std::optional<std::uint64_t> publish_time_ns, std::string_view origin_zid);

    // Note a topic that is advertised but has not published.
    //
    // The one thing traffic can never tell you, and the reason recording an
    // advertisement set is worth doing: after the fact, "this topic produced
    // nothing" and "this topic was not running" look identical in a file that
    // only holds messages. Called repeatedly with the whole current set; already
    // known topics are ignored.
    void noteAdvertised(std::string_view key, std::string_view schema_name);

    // Messages the recorder could not keep up with. Ends up in metadata.yaml and
    // in `bag info`.
    void noteDropped(std::uint64_t count);

    // Finalises the current part, writes metadata.yaml, and closes. Safe to call
    // twice. The destructor calls it, but a caller that wants to know whether it
    // worked has to call it explicitly.
    bool close();

    // The index as it currently stands. Complete only after close().
    const bag_metadata_t& metadata() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bag

#endif  // BAG_WRITER_H_
