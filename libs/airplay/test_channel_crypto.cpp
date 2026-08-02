// SPDX-License-Identifier: GPL-3.0-or-later
//
// The framed transport under AirPlay's two encrypted channels. The failure this
// pins down is the one with no diagnostic on hardware: the frame counters are
// shared state, so a channel that gets them out of step authenticates nothing
// afterwards and there is no way to tell that from a wrong key.
#include "airplay/channel_crypto.h"

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

airplay::Bytes key(uint8_t fill)
{
    return airplay::Bytes(32, fill);
}

// Two ends of one channel: what A seals, B opens.
struct Pair
{
    airplay::ChannelCrypto a;
    airplay::ChannelCrypto b;

    Pair()
    {
        // B's inbound is A's outbound and vice versa -- the crossover that is
        // easy to get backwards, and which looks exactly like a bad key.
        a.activate(key(0x11), key(0x22));
        b.activate(key(0x22), key(0x11));
    }
};

}  // namespace

int main()
{
    using airplay::Bytes;
    using airplay::ChannelCrypto;

    {
        ChannelCrypto channel;
        expect(!channel.active(), "a fresh channel is inactive");
        channel.activate(key(1), key(2));
        expect(channel.active(), "and active once keyed");
        channel.deactivate();
        expect(!channel.active(), "and inactive again when torn down");
    }

    // A single frame round trip.
    {
        Pair p;
        const Bytes message{'h', 'e', 'l', 'l', 'o'};
        Bytes wire = p.a.seal(message);
        expect(wire.size() == 2 + message.size() + 16, "frame is length + ciphertext + tag");
        expect(wire[0] == message.size() && wire[1] == 0, "length prefix is little endian");

        Bytes plain;
        expect(p.b.open(wire, plain), "the peer opens it");
        expect(plain == message, "and recovers the plaintext");
        expect(wire.empty(), "consuming the frame");
    }

    // Successive frames advance the nonce: identical plaintext must not produce
    // identical ciphertext, or the counter is not being used at all.
    {
        Pair p;
        const Bytes message{1, 2, 3};
        const Bytes first = p.a.seal(message);
        const Bytes second = p.a.seal(message);
        expect(first != second, "the same plaintext seals differently each time");

        Bytes wire = first;
        wire.insert(wire.end(), second.begin(), second.end());
        Bytes plain;
        expect(p.b.open(wire, plain), "both frames open in one pass");
        expect(plain.size() == 6, "and both plaintexts are appended in order");
    }

    // A message over the frame cap is split, and reassembles transparently.
    {
        Pair p;
        Bytes big(ChannelCrypto::kMaxFramePlaintext * 2 + 5);
        std::iota(big.begin(), big.end(), 0);

        Bytes wire = p.a.seal(big);
        expect(wire.size() == big.size() + 3 * (2 + 16), "split into three frames");

        Bytes plain;
        expect(p.b.open(wire, plain), "all three open");
        expect(plain == big, "and reassemble to the original");
    }

    // A frame arriving in pieces waits rather than failing.
    {
        Pair p;
        const Bytes message{9, 8, 7, 6};
        const Bytes wire = p.a.seal(message);

        Bytes partial(wire.begin(), wire.end() - 3);
        Bytes plain;
        expect(p.b.open(partial, plain), "a partial frame is not an error");
        expect(plain.empty(), "and yields nothing yet");
        expect(partial.size() == wire.size() - 3, "leaving the bytes buffered");

        partial.insert(partial.end(), wire.end() - 3, wire.end());
        expect(p.b.open(partial, plain), "the rest completes it");
        expect(plain == message, "recovering the plaintext");
    }

    // Frames replayed or delivered out of order must fail, not silently decode.
    {
        Pair p;
        const Bytes first = p.a.seal({1});
        const Bytes second = p.a.seal({2});

        Bytes out_of_order = second;
        Bytes plain;
        expect(!p.b.open(out_of_order, plain), "a frame out of order fails to authenticate");

        Pair q;
        Bytes wire = q.a.seal({1});
        Bytes replay = wire;
        Bytes ignored;
        expect(q.b.open(wire, ignored), "the first delivery opens");
        expect(!q.b.open(replay, ignored), "and the replay of it does not");
        (void)first;
    }

    // A corrupted tag is rejected.
    {
        Pair p;
        Bytes wire = p.a.seal({4, 5, 6});
        wire.back() ^= 0xFF;
        Bytes plain;
        expect(!p.b.open(wire, plain), "a tampered tag fails to authenticate");
    }

    if (failures == 0)
    {
        SPDLOG_INFO("channel crypto tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
