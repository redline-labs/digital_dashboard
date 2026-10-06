// SPDX-License-Identifier: GPL-3.0-or-later
//
// FileTransferAssembler: the phone's artwork arrives whole, and a transfer that
// claims or grows to more than any artwork is does not take the memory with it.
#include "iap2/file_transfer.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

using iap2::FileTransferAssembler;
using Bytes = std::vector<uint8_t>;

Bytes datagram(uint8_t id, uint8_t control, Bytes data = {})
{
    Bytes out{id, control};
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

Bytes setup(uint8_t id, uint64_t size)
{
    Bytes out{id, FileTransferAssembler::kSetup};
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        out.push_back(static_cast<uint8_t>(size >> shift));
    }
    return out;
}

bool replied(const FileTransferAssembler::Result& result, uint8_t id, uint8_t control)
{
    for (const auto& reply : result.replies)
    {
        if (reply == Bytes{id, control})
        {
            return true;
        }
    }
    return false;
}

}  // namespace

int main()
{
    // A whole file, in three pieces.
    {
        FileTransferAssembler ft(16, 64);
        expect(replied(ft.handle(setup(7, 9)), 7, FileTransferAssembler::kStart), "setup is answered with start");
        expect(!ft.handle(datagram(7, FileTransferAssembler::kFirstData, {1, 2, 3})).completed, "first piece");
        ft.handle(datagram(7, FileTransferAssembler::kData, {4, 5, 6}));
        const auto done = ft.handle(datagram(7, FileTransferAssembler::kLastData, {7, 8, 9}));
        expect(done.completed && *done.completed == Bytes({1, 2, 3, 4, 5, 6, 7, 8, 9}),
               "the pieces arrive whole and in order");
        expect(replied(done, 7, FileTransferAssembler::kSuccess), "and success is reported");
        expect(ft.bufferedBytes() == 0, "nothing is held afterwards");

        const auto single = ft.handle(datagram(8, FileTransferAssembler::kFirstAndOnlyData, {42}));
        expect(single.completed && *single.completed == Bytes({42}), "a one-piece file arrives");
    }

    // Too big, declared: refused at setup.
    {
        FileTransferAssembler ft(16, 64);
        const auto refused = ft.handle(setup(3, 1000));
        expect(replied(refused, 3, FileTransferAssembler::kCancel) &&
                   !replied(refused, 3, FileTransferAssembler::kStart),
               "a transfer declared over the limit is cancelled, not started");
    }

    // Too big, undeclared: cancelled as it passes the limit.
    {
        FileTransferAssembler ft(16, 64);
        ft.handle(setup(4, 8));  // claims 8, sends more
        ft.handle(datagram(4, FileTransferAssembler::kFirstData, Bytes(10, 1)));
        const auto over = ft.handle(datagram(4, FileTransferAssembler::kData, Bytes(10, 2)));
        expect(replied(over, 4, FileTransferAssembler::kCancel), "a transfer that grows past the limit is cancelled");
        expect(ft.bufferedBytes() == 0, "and its bytes are released");
        const auto late = ft.handle(datagram(4, FileTransferAssembler::kLastData, {1}));
        expect(late.completed && late.completed->size() == 1,
               "what follows a cancel is not glued onto the dropped data");

        const auto huge = ft.handle(datagram(5, FileTransferAssembler::kFirstAndOnlyData, Bytes(17, 0)));
        expect(!huge.completed && replied(huge, 5, FileTransferAssembler::kCancel),
               "a single oversized piece is refused");
    }

    // Many transfers open at once cannot add up past the total.
    {
        FileTransferAssembler ft(16, 40);
        FileTransferAssembler::Result last;
        for (uint8_t id = 0; id < 4; ++id)
        {
            ft.handle(setup(id, 16));
            last = ft.handle(datagram(id, FileTransferAssembler::kFirstData, Bytes(12, id)));
        }
        expect(ft.bufferedBytes() == 36, "open transfers together stay within the total");
        expect(replied(last, 3, FileTransferAssembler::kCancel), "the one that would pass it is cancelled");
    }

    // Malformed datagrams are ignored, not acted on.
    {
        FileTransferAssembler ft;
        expect(ft.handle({}).replies.empty() && ft.handle({1}).replies.empty(),
               "a datagram shorter than its header does nothing");
        expect(ft.handle(datagram(1, 0x7F)).replies.empty(), "an unknown control byte does nothing");
        const auto short_setup = ft.handle(datagram(2, FileTransferAssembler::kSetup, {0, 1}));
        expect(replied(short_setup, 2, FileTransferAssembler::kStart),
               "a setup without a size is started, and bounded as it arrives");
    }

    if (failures == 0)
    {
        SPDLOG_INFO("all file transfer checks passed");
    }
    return failures == 0 ? 0 : 1;
}
