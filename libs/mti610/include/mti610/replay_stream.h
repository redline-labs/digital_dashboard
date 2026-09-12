// SPDX-License-Identifier: GPL-3.0-or-later
//
// A ByteStream over a file of captured bytes.
//
// The whole decode path -- framing, resynchronisation, the item walk, the
// payload parsers, the schema conversion, the publishing -- can be exercised
// with no device and no serial port. With no MTi-610 available at all, this is
// not a convenience here the way it is for the BD992: it is the only way to
// run the node end to end.
//
// The chunk size is deliberately configurable and deliberately small by
// default. Handing the framer a whole file in one call would test a case that
// never happens on a serial port; handing it seven bytes at a time tests the
// one that always does.

#ifndef MTI610_REPLAY_STREAM_H
#define MTI610_REPLAY_STREAM_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "mti610/byte_stream.h"
#include "mti610/error.h"

namespace mti610
{

class ReplayStream final : public ByteStream
{
  public:
    struct Options
    {
        // Bytes handed over per recvSome(). A typical MTData2 at 100 Hz is
        // under a hundred bytes, so the default splits most messages across
        // several reads -- which is the point.
        std::size_t chunkSize { 64 };

        // Start again at the beginning when the file runs out, rather than
        // reporting the stream closed.
        bool loop { false };

        // Wall-clock delay per chunk. Zero replays as fast as the consumer can
        // take it, which is what a test wants; a node feeding a dashboard
        // wants something closer to real time.
        unsigned chunkDelayMs { 0 };
    };

    static Result<std::unique_ptr<ReplayStream>> open(const std::string& path, Options options);

    // For tests: replay bytes already in memory.
    static std::unique_ptr<ReplayStream> fromBytes(std::vector<std::uint8_t> bytes,
                                                   Options options);

    bool sendAll(std::span<const std::uint8_t> data) override;
    ssize_t recvSome(std::span<std::uint8_t> out, unsigned timeoutMs) override;
    bool isOpen() const override;
    void close() override;

    std::size_t size() const { return mBytes.size(); }

  private:
    ReplayStream(std::vector<std::uint8_t> bytes, Options options);

    std::vector<std::uint8_t> mBytes;
    Options mOptions;
    std::size_t mOffset { 0 };
    bool mOpen { true };
};

} // namespace mti610

#endif // MTI610_REPLAY_STREAM_H
