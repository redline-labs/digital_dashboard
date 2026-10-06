// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef IAP2_FILE_TRANSFER_H_
#define IAP2_FILE_TRANSFER_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace iap2
{

// Reassembles the phone's iAP2 file transfers (album artwork), one datagram at
// a time: [identifier][control][data...].
//
// Bounded. The phone says how big a file is in its setup datagram and then
// sends it in pieces; nothing stopped a transfer that declared, or grew to,
// more than any artwork is, from growing until memory ran out. A transfer
// declared over the limit is refused at setup, one that grows past it is
// cancelled, and so is one that would take every open transfer past the total.
class FileTransferAssembler
{
  public:
    static constexpr size_t kMaxFileBytes = 4 * 1024 * 1024;
    static constexpr size_t kMaxTotalBytes = 8 * 1024 * 1024;

    // Control bytes.
    static constexpr uint8_t kStart = 0x01;
    static constexpr uint8_t kCancel = 0x02;
    static constexpr uint8_t kSetup = 0x04;
    static constexpr uint8_t kSuccess = 0x05;
    static constexpr uint8_t kData = 0x00;
    static constexpr uint8_t kLastData = 0x40;
    static constexpr uint8_t kFirstData = 0x80;
    static constexpr uint8_t kFirstAndOnlyData = 0xC0;

    struct Result
    {
        // Datagrams to send back, in order.
        std::vector<std::vector<uint8_t>> replies;
        // A file that has just arrived whole.
        std::optional<std::vector<uint8_t>> completed;
    };

    FileTransferAssembler(size_t max_file_bytes = kMaxFileBytes,
                          size_t max_total_bytes = kMaxTotalBytes);

    Result handle(const std::vector<uint8_t>& datagram);

    // Bytes held across every open transfer.
    size_t bufferedBytes() const;

  private:
    // Drops the transfer and tells the phone.
    void cancel(uint8_t id, Result& result);

    size_t max_file_bytes_;
    size_t max_total_bytes_;
    std::map<uint8_t, std::vector<uint8_t>> transfers_;
};

}  // namespace iap2

#endif  // IAP2_FILE_TRANSFER_H_
