// SPDX-License-Identifier: GPL-3.0-or-later

#include "bd992/replay_stream.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

namespace bd992
{

ReplayStream::ReplayStream(std::vector<std::uint8_t> bytes, Options options) :
    mBytes(std::move(bytes)),
    mOptions(options)
{
    if (mOptions.chunkSize == 0)
    {
        mOptions.chunkSize = 1;
    }
}

Result<std::unique_ptr<ReplayStream>> ReplayStream::open(const std::string& path, Options options)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return not_found("cannot open " + path);
    }

    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());

    if (bytes.empty())
    {
        // An empty capture would otherwise present as an immediate clean
        // end-of-stream, which looks exactly like a receiver that connected
        // and said nothing.
        return io_error(path + " is empty");
    }

    return std::unique_ptr<ReplayStream>(new ReplayStream(std::move(bytes), options));
}

std::unique_ptr<ReplayStream> ReplayStream::fromBytes(std::vector<std::uint8_t> bytes, Options options)
{
    return std::unique_ptr<ReplayStream>(new ReplayStream(std::move(bytes), options));
}

bool ReplayStream::sendAll(std::span<const std::uint8_t> data)
{
    // A capture cannot answer. Reported as success rather than failure so a
    // caller that writes a command during replay is not torn down by it --
    // the reply simply never comes, and the caller's own timeout handles it.
    (void)data;
    return mOpen;
}

ssize_t ReplayStream::recvSome(std::span<std::uint8_t> out, unsigned timeoutMs)
{
    (void)timeoutMs;

    if (!mOpen)
    {
        return -1;
    }

    if (mOffset >= mBytes.size())
    {
        if (!mOptions.loop)
        {
            // End of the capture, reported the same way a closed peer is.
            mOpen = false;
            return -1;
        }
        mOffset = 0;
    }

    if (mOptions.chunkDelayMs != 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(mOptions.chunkDelayMs));
    }

    const std::size_t take = std::min({ mOptions.chunkSize, out.size(), mBytes.size() - mOffset });
    std::memcpy(out.data(), mBytes.data() + mOffset, take);
    mOffset += take;

    return static_cast<ssize_t>(take);
}

bool ReplayStream::isOpen() const
{
    return mOpen;
}

void ReplayStream::close()
{
    mOpen = false;
}

} // namespace bd992
