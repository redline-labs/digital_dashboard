// SPDX-License-Identifier: GPL-3.0-or-later
#include "iap2/file_transfer.h"

#include <spdlog/spdlog.h>

namespace iap2
{

FileTransferAssembler::FileTransferAssembler(size_t max_file_bytes, size_t max_total_bytes) :
    max_file_bytes_(max_file_bytes), max_total_bytes_(max_total_bytes)
{
}

size_t FileTransferAssembler::bufferedBytes() const
{
    size_t total = 0;
    for (const auto& [id, data] : transfers_)
    {
        (void)id;
        total += data.size();
    }
    return total;
}

void FileTransferAssembler::cancel(uint8_t id, Result& result)
{
    transfers_.erase(id);
    result.replies.push_back({id, kCancel});
}

FileTransferAssembler::Result FileTransferAssembler::handle(const std::vector<uint8_t>& datagram)
{
    Result result;
    if (datagram.size() < 2)
    {
        return result;
    }
    const uint8_t id = datagram[0];
    const uint8_t control = datagram[1];
    const auto data_begin = datagram.begin() + 2;
    const size_t data_size = datagram.size() - 2;

    switch (control)
    {
        case kSetup:
        {
            // The declared size, when there is one: eight bytes, big endian.
            if (data_size >= 8)
            {
                uint64_t declared = 0;
                for (size_t i = 0; i < 8; ++i)
                {
                    declared = (declared << 8) | datagram[2 + i];
                }
                if (declared > max_file_bytes_)
                {
                    SPDLOG_WARN("[iap2] file transfer {} declares {} bytes, over the {} limit; refusing",
                                id, declared, max_file_bytes_);
                    cancel(id, result);
                    return result;
                }
            }
            transfers_[id] = {};
            result.replies.push_back({id, kStart});
            return result;
        }
        case kCancel:
            transfers_.erase(id);
            return result;
        case kFirstAndOnlyData:
            if (data_size > max_file_bytes_)
            {
                cancel(id, result);
                return result;
            }
            transfers_.erase(id);
            result.completed = std::vector<uint8_t>(data_begin, datagram.end());
            result.replies.push_back({id, kSuccess});
            return result;
        case kFirstData:
        case kData:
        case kLastData:
        {
            std::vector<uint8_t>& buffer = transfers_[id];
            if (control == kFirstData)
            {
                buffer.clear();
            }
            if (buffer.size() + data_size > max_file_bytes_ ||
                bufferedBytes() + data_size > max_total_bytes_)
            {
                SPDLOG_WARN("[iap2] file transfer {} grew past its limit; cancelling", id);
                cancel(id, result);
                return result;
            }
            buffer.insert(buffer.end(), data_begin, datagram.end());
            if (control == kLastData)
            {
                result.completed = std::move(buffer);
                transfers_.erase(id);
                result.replies.push_back({id, kSuccess});
            }
            return result;
        }
        default:
            return result;
    }
}

}  // namespace iap2
