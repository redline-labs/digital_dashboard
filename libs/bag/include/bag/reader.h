#ifndef BAG_READER_H_
#define BAG_READER_H_

#include "bag/metadata.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bag
{

// One recorded message, as handed to a reader's callback.
struct BagMessage
{
    // Views into buffers owned by the reader, valid only for the duration of the
    // callback -- the payload in particular points into a decompressed chunk
    // that the next message may replace. Copy anything you keep.
    std::string_view key;

    // The registry name ("EngineRpm"), read back from the channel metadata --
    // NOT the MCAP Schema record's name, which is capnp's qualified form. Empty
    // if the recording did not carry one.
    std::string_view schema;

    std::span<const std::uint8_t> payload;

    // Nanoseconds since the UNIX epoch. `log_time` is when the recorder saw it;
    // `publish_time` is when the publisher's session stamped it, or equal to
    // log_time when the sample arrived unstamped. metadata().unstamped_messages
    // says how often the latter happened.
    std::uint64_t log_time_ns = 0;
    std::uint64_t publish_time_ns = 0;
};

// Reads a bag directory as one continuous, time-ordered message stream.
//
// The split into parts is invisible here. That is the whole point of the
// directory layout: rolling exists so a crash costs one part rather than the
// recording, and so a multi-gigabyte capture is a set of copyable files -- none
// of which should be a consumer's problem.
class BagReader
{
  public:
    explicit BagReader(std::string directory);
    ~BagReader();

    BagReader(const BagReader&) = delete;
    BagReader& operator=(const BagReader&) = delete;

    // False when the directory has no readable metadata.yaml. Note that a bag
    // with a DAMAGED part is still valid -- see problems().
    bool isValid() const;

    const bag_metadata_t& metadata() const;

    // Anything wrong with the recording that is not fatal: a part missing from
    // disk, a part whose summary is absent because its writer died. Reported
    // rather than thrown, because a recording with a torn tail is still worth
    // reading right up to the tear -- and that is the common case after a crash,
    // which is exactly when the data matters most.
    const std::vector<std::string>& problems() const;

    // Visit every message with `start_ns <= log_time <= end_ns`, in log_time
    // order.
    //
    // Return false from the callback to stop early. Returns false itself only if
    // a part could not be opened at all.
    //
    // Seeking is real: MCAP's ChunkIndex records let the underlying reader skip
    // whole chunks whose time range falls outside the window, and parts outside
    // it are never opened. A range near the end of a large recording does not
    // pay for the beginning of it.
    bool forEach(std::uint64_t start_ns, std::uint64_t end_ns,
                 const std::function<bool(const BagMessage&)>& callback);

    bool forEach(const std::function<bool(const BagMessage&)>& callback)
    {
        return forEach(0, std::numeric_limits<std::uint64_t>::max(), callback);
    }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace bag

#endif  // BAG_READER_H_
