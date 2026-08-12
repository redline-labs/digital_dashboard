// SPDX-License-Identifier: GPL-3.0-or-later
//
// A ByteStream over a file of captured bytes.
//
// This is the `trc:` replay of the GNSS stack, and it exists for the same
// reason: the whole decode path -- framing, resynchronisation, page assembly,
// record parsing, schema conversion, publishing -- can then be exercised with
// no receiver, no antenna and no sky. A minute of `--dump-gsof` from a vehicle
// becomes a regression test that runs on a laptop.
//
// The chunk size is deliberately configurable and deliberately small by
// default. Handing the framer the whole file in one call would test a case
// that never happens on a socket; handing it seven bytes at a time tests the
// one that always does.

#ifndef BD992_REPLAY_STREAM_H
#define BD992_REPLAY_STREAM_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "bd992/byte_stream.h"
#include "bd992/error.h"

namespace bd992
{

class ReplayStream final : public ByteStream
{
  public:
    struct Options
    {
        // Bytes handed over per recvSome(). A GSOF packet is up to 261 bytes,
        // so the default splits most packets across several reads.
        std::size_t chunkSize { 64 };

        // Start again at the beginning when the file runs out, rather than
        // reporting the stream closed.
        bool loop { false };

        // Wall-clock delay per chunk. Zero replays as fast as the consumer
        // can take it, which is what a test wants; a node demonstrating a
        // dashboard wants something closer to real time.
        unsigned chunkDelayMs { 0 };
    };

    static Result<std::unique_ptr<ReplayStream>> open(const std::string& path, Options options);

    // For tests: replay bytes already in memory.
    static std::unique_ptr<ReplayStream> fromBytes(std::vector<std::uint8_t> bytes, Options options);

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

} // namespace bd992

#endif // BD992_REPLAY_STREAM_H
