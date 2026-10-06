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

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

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

// A UDP socket on [::1] standing in for the phone's mic port.
struct PhonePort
{
    int fd = -1;
    uint16_t port = 0;

    PhonePort()
    {
        fd = ::socket(AF_INET6, SOCK_DGRAM, 0);
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_loopback;
        ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        socklen_t len = sizeof(addr);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin6_port);
    }
    ~PhonePort() { ::close(fd); }

    // The next packet, or empty after a second.
    airplay::Bytes receive() const
    {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 1000) <= 0)
        {
            return {};
        }
        airplay::Bytes packet(2048);
        const ssize_t n = ::recv(fd, packet.data(), packet.size(), 0);
        packet.resize(n > 0 ? static_cast<size_t>(n) : 0);
        return packet;
    }
};

// Whether `packet` opens under the key the phone derives for `connection_id`.
bool opensFor(const airplay::Bytes& packet, const airplay::Bytes& shared, uint64_t connection_id)
{
    if (packet.size() < 12 + 16 + 8)
    {
        return false;
    }
    const airplay::Bytes stream_key = airplay::crypto::hkdfSha512(
        shared, "DataStream-Salt" + std::to_string(connection_id), "DataStream-Input-Encryption-Key", 32);
    airplay::Bytes nonce(4, 0);
    nonce.insert(nonce.end(), packet.end() - 8, packet.end());
    const airplay::Bytes aad(packet.begin() + 4, packet.begin() + 12);
    const airplay::Bytes sealed(packet.begin() + 12, packet.end() - 8);
    return airplay::crypto::chachaOpen(stream_key, nonce, sealed, aad).has_value();
}

uint16_t sequenceOf(const airplay::Bytes& packet)
{
    return packet.size() >= 4 ? static_cast<uint16_t>((packet[2] << 8) | packet[3]) : 0xFFFF;
}

// The phone sets up the main audio stream again -- a call placed from Siri --
// without tearing the first down. The uplink must follow the new key and port;
// left on the old ones, the call is one-way.
void testReSetup()
{
    using airplay::Bytes;
    airplay::MicUplink uplink;
    int ups = 0;
    uplink.setStatusHandler([&ups](bool active, uint32_t, uint8_t) { ups += active ? 1 : 0; });

    const Bytes shared(32, 0x33);
    const airplay::MicUplink::Peer peer{"::1", 0};
    const Bytes frame(4 * 2, 0x01);  // four mono samples, framesPerPacket 4

    PhonePort siri;
    uplink.start(peer, siri.port, shared, 16000, 1, 100, 1, 4);
    uplink.feed(frame);
    const Bytes first = siri.receive();
    expect(opensFor(first, shared, 1), "the uplink sends under the stream's key");

    uplink.start(peer, siri.port, shared, 16000, 1, 100, 1, 4);
    uplink.feed(frame);
    const Bytes repeat = siri.receive();
    expect(ups == 1 && sequenceOf(repeat) == 1,
           "an identical SETUP leaves the running uplink alone");

    PhonePort call;
    uplink.start(peer, call.port, shared, 24000, 1, 100, 2, 4);
    uplink.feed(frame);
    const Bytes moved = call.receive();
    expect(ups == 2, "a SETUP with a new connection id and rate brings the uplink up again");
    expect(opensFor(moved, shared, 2), "under the new stream's key");
    expect(!opensFor(moved, shared, 1), "not the old one");
    expect(sequenceOf(moved) == 0, "starting its sequence afresh");
    uplink.stop();
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

    testReSetup();

    if (failures == 0)
    {
        SPDLOG_INFO("mic uplink tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
