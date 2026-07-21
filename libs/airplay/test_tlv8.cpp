// SPDX-License-Identifier: GPL-3.0-or-later
#include "airplay/tlv8.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <numeric>

namespace
{

int failures = 0;

void expect(bool condition, const char* what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

}  // namespace

int main()
{
    using airplay::tlv8::decode;
    using airplay::tlv8::encode;
    using airplay::tlv8::find;
    using airplay::tlv8::Item;

    // Simple round trip.
    {
        const std::vector<Item> items = {{0x01, {0xde, 0xad}}, {0x06, {0x03}}};
        const auto decoded = decode(encode(items));
        expect(decoded == items, "simple round trip");
    }

    // Empty value.
    {
        const std::vector<Item> items = {{0x0b, {}}};
        const auto decoded = decode(encode(items));
        expect(decoded == items, "empty value round trip");
    }

    // Fragmentation: > 255 bytes must split into fragments and merge back.
    {
        std::vector<uint8_t> big(600);
        std::iota(big.begin(), big.end(), 0);
        const std::vector<Item> items = {{0x09, big}, {0x01, {0x42}}};

        const auto encoded = encode(items);
        // 600 bytes -> fragments of 255 + 255 + 90, each with 2 bytes header.
        expect(encoded.size() == 600 + 3 * 2 + 1 + 2, "fragment encoding size");

        const auto decoded = decode(encoded);
        expect(decoded == items, "fragmented round trip");
    }

    // Exactly 255 bytes: single fragment, must not swallow a following
    // distinct item of the same type... but a same-type adjacent item is
    // indistinguishable from a continuation by design (HAP requires a
    // different type or 0-length separator between same-type items).
    {
        std::vector<uint8_t> exact(255, 0xaa);
        const std::vector<Item> items = {{0x05, exact}, {0x06, {0x01}}};
        const auto decoded = decode(encode(items));
        expect(decoded == items, "exact 255 round trip");
    }

    // find()
    {
        const std::vector<Item> items = {{0x01, {0x11}}, {0x02, {0x22}}};
        const auto* v = find(items, 0x02);
        expect(v != nullptr && (*v)[0] == 0x22, "find existing");
        expect(find(items, 0x7f) == nullptr, "find missing");
    }

    if (failures == 0)
    {
        SPDLOG_INFO("tlv8 tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
