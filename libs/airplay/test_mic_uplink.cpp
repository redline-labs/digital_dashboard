// SPDX-License-Identifier: GPL-3.0-or-later
//
// The microphone uplink's wire format.
//
// Nothing here errors when it is wrong. Captured audio simply reaches the phone
// as clicks, noise, or silence, and the only way to tell which of half a dozen
// details is at fault is to have checked them separately.
#include "airplay/mic_uplink.h"

#include "airplay/crypto.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <numeric>
#include <string>

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

airplay::Bytes key()
{
    return airplay::Bytes(32, 0x5A);
}

}  // namespace

int main()
{
    using airplay::Bytes;
    namespace mic = airplay::mic;

    // S16LE in, big-endian out.
    {
        Bytes pcm{0x34, 0x12, 0x78, 0x56};
        mic::swapSampleEndianness(pcm);
        expect(pcm == Bytes({0x12, 0x34, 0x56, 0x78}), "each sample's bytes are swapped");

        // Twice is the identity, which is the cheap way to say it swaps within
        // samples rather than reversing the buffer.
        mic::swapSampleEndianness(pcm);
        expect(pcm == Bytes({0x34, 0x12, 0x78, 0x56}), "swapping twice restores the input");

        // An odd trailing byte is half a sample whose other half has not
        // arrived; touching it would corrupt the sample when it does.
        Bytes odd{0x11, 0x22, 0x33};
        mic::swapSampleEndianness(odd);
        expect(odd == Bytes({0x22, 0x11, 0x33}), "a trailing odd byte is left alone");

        Bytes empty;
        mic::swapSampleEndianness(empty);
        expect(empty.empty(), "an empty buffer is fine");
    }

    // Frame sizing. Stereo 16-bit at 20 ms of 44.1 kHz is the common case.
    {
        expect(mic::frameBytes(882, 2) == 882 * 4, "stereo frame bytes");
        expect(mic::frameBytes(160, 1) == 320, "mono frame bytes");
        expect(mic::frameBytes(0, 2) == 0, "a zero frame length is unusable");
        expect(mic::frameBytes(160, 0) == 0, "a zero channel count is unusable");
    }

    // The packet layout, and that it can be opened again.
    {
        Bytes body(64);
        std::iota(body.begin(), body.end(), 0);

        const Bytes packet = mic::buildPacket(body, 100, 0x1234, 0xAABBCCDD, 7, key());
        expect(packet.size() == 12 + body.size() + 16 + 8,
               "header + sealed body + tag + nonce tail");

        expect(packet[0] == 0x80, "RTP version 2, no padding or extensions");
        expect(packet[1] == 100, "payload type");
        expect(packet[2] == 0x12 && packet[3] == 0x34, "sequence is big endian");
        expect(packet[4] == 0xAA && packet[5] == 0xBB && packet[6] == 0xCC && packet[7] == 0xDD,
               "timestamp is big endian");
        expect(packet[8] == 0 && packet[9] == 0 && packet[10] == 0 && packet[11] == 0,
               "SSRC is zero for a CarPlay input stream");

        // The trailing nonce must be the low 8 bytes of the counter nonce, or
        // the phone derives a different one and every packet fails to open.
        const Bytes nonce = airplay::crypto::nonce64(7);
        const Bytes tail(packet.end() - 8, packet.end());
        expect(tail == Bytes(nonce.end() - 8, nonce.end()), "the nonce tail matches nonce64");

        // Open it the way the phone would: AAD is the timestamp and SSRC, not
        // the whole header -- covering the sequence number instead is a
        // plausible mistake that fails only on the far side.
        const Bytes aad(packet.begin() + 4, packet.begin() + 12);
        const Bytes sealed(packet.begin() + 12, packet.end() - 8);
        const auto opened = airplay::crypto::chachaOpen(key(), nonce, sealed, aad);
        expect(opened.has_value(), "the packet opens with the same key, nonce and AAD");
        expect(opened && *opened == body, "and yields the body unchanged");

        // The AAD really is only those eight bytes.
        const Bytes wrong_aad(packet.begin(), packet.begin() + 12);
        expect(!airplay::crypto::chachaOpen(key(), nonce, sealed, wrong_aad).has_value(),
               "the whole header is not the AAD");
    }

    // A payload type is seven bits; the top bit is the marker.
    {
        const Bytes packet = mic::buildPacket({0, 0}, 0xFF, 0, 0, 0, key());
        expect(packet[1] == 0x7F, "the payload type is masked to seven bits");
    }

    // Successive packets differ even for identical audio, because the nonce
    // advances. Identical ciphertext would mean the counter is not being used.
    {
        const Bytes body(32, 0x41);
        const Bytes first = mic::buildPacket(body, 100, 0, 0, 0, key());
        const Bytes second = mic::buildPacket(body, 100, 1, 882, 1, key());
        expect(Bytes(first.begin() + 12, first.end() - 8) !=
                   Bytes(second.begin() + 12, second.end() - 8),
               "the same audio seals differently under successive nonces");
    }

    // The framing rule the accumulator implements: whole frames only, with the
    // remainder held back. Padding a short frame injects a click at every
    // capture boundary that does not divide evenly.
    {
        const size_t frame = mic::frameBytes(4, 2);  // 16 bytes
        Bytes accum;
        size_t emitted = 0;

        // Feed sizes that never line up with the frame.
        for (int i = 0; i < 10; ++i)
        {
            const Bytes chunk(7, static_cast<uint8_t>(i));
            accum.insert(accum.end(), chunk.begin(), chunk.end());
            while (accum.size() >= frame)
            {
                accum.erase(accum.begin(), accum.begin() + static_cast<long>(frame));
                ++emitted;
            }
        }
        expect(emitted == (10 * 7) / frame, "only whole frames are emitted");
        expect(accum.size() == (10 * 7) % frame, "and the remainder is carried forward");
        expect(accum.size() < frame, "never more than a frame is held back");
    }

    if (failures == 0)
    {
        SPDLOG_INFO("mic uplink tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
