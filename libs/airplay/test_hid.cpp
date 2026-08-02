// SPDX-License-Identifier: GPL-3.0-or-later
//
// The HID devices the accessory advertises and the reports it sends on them.
//
// Nothing here can be checked against a phone without hardware, so what these
// tests pin down is the part that is checkable: that each descriptor is
// structurally a valid HID report descriptor whose declared report size matches
// the reports we actually build. A descriptor and a report that disagree is the
// failure mode with no symptom -- the phone accepts the device, then silently
// discards every report on it.
#include "airplay/hid.h"

#include "plist/binary.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
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

// Walks a report descriptor's short items, summing the Input item sizes so the
// declared report length can be compared with what we send. Returns -1 if the
// item stream is malformed or the collections do not balance.
int descriptorInputBits(const airplay::hid::Bytes& descriptor)
{
    int total_bits = 0;
    int report_size = 0;
    int report_count = 0;
    int depth = 0;

    size_t i = 0;
    while (i < descriptor.size())
    {
        const uint8_t prefix = descriptor[i];
        // Short items only: [tag(4) type(2) size(2)], where size 3 means 4.
        const uint8_t size_code = prefix & 0x03;
        const size_t data_len = size_code == 3 ? 4 : size_code;
        if (i + 1 + data_len > descriptor.size())
        {
            return -1;  // truncated item
        }

        uint32_t data = 0;
        for (size_t b = 0; b < data_len; ++b)
        {
            data |= static_cast<uint32_t>(descriptor[i + 1 + b]) << (8 * b);
        }

        switch (prefix & 0xFC)
        {
            case 0x74:  // Global: Report Size
                report_size = static_cast<int>(data);
                break;
            case 0x94:  // Global: Report Count
                report_count = static_cast<int>(data);
                break;
            case 0x80:  // Main: Input
                total_bits += report_size * report_count;
                break;
            case 0xA0:  // Main: Collection
                ++depth;
                break;
            case 0xC0:  // Main: End Collection
                --depth;
                if (depth < 0)
                {
                    return -1;
                }
                break;
            default:
                break;  // usages, logical min/max: not part of the report size
        }
        i += 1 + data_len;
    }
    return depth == 0 ? total_bits : -1;
}

void checkDescriptor(const char* name, const airplay::hid::Bytes& descriptor,
                     size_t report_bytes)
{
    const int bits = descriptorInputBits(descriptor);
    expect(bits > 0, std::string(name) + " descriptor parses and its collections balance");
    expect(bits == static_cast<int>(report_bytes) * 8,
           std::string(name) + " descriptor declares " + std::to_string(report_bytes) +
               " report bytes (got " + std::to_string(bits) + " bits)");
}

}  // namespace

int main()
{
    using namespace airplay::hid;

    // Device ids are hex, lower case, unpadded -- the phone matches the /info
    // entry against the report's uuid as a string, so the two renderings of the
    // same id have to agree exactly.
    {
        expect(uidToString(kTouchUid) == "2a2a2a2a", "touch uid renders as hex");
        expect(uidToString(kKnobUid) == "2a2a2a2b", "knob uid renders as hex");
        expect(uidToString(0x1) == "1", "small uid is not zero padded");
        expect(uidToString(0) == "0", "zero uid renders as a single digit");
    }

    // Each descriptor against the report it is paired with.
    {
        checkDescriptor("touch", touchDescriptor(800, 600), touchReport({}).size());
        checkDescriptor("knob", knobDescriptor(), knobReport({}).size());
        checkDescriptor("media", mediaDescriptor(), mediaReport(MediaKey::None).size());
        checkDescriptor("telephony", telephonyDescriptor(),
                        telephonyReport(TelephonyKey::None).size());
    }

    // The touch descriptor carries the display size, so a resize has to reach
    // the logical maximums or the phone maps every touch to the wrong place.
    {
        const Bytes descriptor = touchDescriptor(0x1234, 0x5678);
        bool found_x = false;
        bool found_y = false;
        for (size_t i = 0; i + 2 < descriptor.size(); ++i)
        {
            // 0x26 is Logical Maximum with a two-byte little-endian payload.
            if (descriptor[i] == 0x26 && descriptor[i + 1] == 0x34 && descriptor[i + 2] == 0x12)
            {
                found_x = true;
            }
            if (descriptor[i] == 0x26 && descriptor[i + 1] == 0x78 && descriptor[i + 2] == 0x56)
            {
                found_y = true;
            }
        }
        expect(found_x && found_y, "touch descriptor carries the display size");
    }

    // Touch reports are always full width, whatever the caller passes: a short
    // report does not match the descriptor and is discarded, not truncated.
    {
        const Bytes none = touchReport({});
        const Bytes one = touchReport({{100, 200, true}});
        const Bytes two = touchReport({{1, 2, true}, {3, 4, true}});
        expect(none.size() == one.size() && one.size() == two.size(),
               "every touch report is the same length");

        // Per finger: [transducer index, touch, X lo, X hi, Y lo, Y hi].
        expect(one[0] == 0 && one[6] == 1, "transducer indices are reported for every slot");
        expect(one[1] == 1 && one[2] == 100 && one[4] == 200, "contact 0 carries its position");
        expect(one[7] == 0, "an absent contact reports not-down");
        expect(two[7] == 1 && two[8] == 3 && two[10] == 4, "contact 1 carries its position");

        const Bytes wide = touchReport({{0x1234, 0x5678, true}});
        expect(wide[2] == 0x34 && wide[3] == 0x12, "X is little endian");
        expect(wide[4] == 0x78 && wide[5] == 0x56, "Y is little endian");

        // More contacts than slots: extras are dropped rather than overrunning.
        const Bytes over = touchReport({{1, 1, true}, {2, 2, true}, {3, 3, true}});
        expect(over.size() == two.size(), "extra contacts do not lengthen the report");
    }

    // Knob: three button bits in byte 0, then signed pan and wheel.
    {
        expect(knobReport({}) == Bytes({0, 0, 0, 0}), "an idle knob reports all clear");
        expect(knobReport({.select = true})[0] == 0x01, "select is bit 0");
        expect(knobReport({.home = true})[0] == 0x02, "home is bit 1");
        expect(knobReport({.back = true})[0] == 0x04, "back is bit 2");
        expect(knobReport({.select = true, .home = true, .back = true})[0] == 0x07,
               "buttons combine");

        const Bytes turned = knobReport({.wheel = -3});
        expect(static_cast<int8_t>(turned[3]) == -3, "a counter-clockwise detent is negative");

        // Out-of-range deltas clamp rather than wrapping, which would turn a
        // fast clockwise spin into a counter-clockwise one.
        expect(static_cast<int8_t>(knobReport({.wheel = 5000})[3]) == 127, "wheel clamps high");
        expect(static_cast<int8_t>(knobReport({.pan_x = -5000})[1]) == -127, "pan clamps low");
    }

    // Media and telephony reports are a single usage index.
    {
        expect(mediaReport(MediaKey::Next) == Bytes({4}), "media key is its usage index");
        expect(telephonyReport(TelephonyKey::Drop) == Bytes({3}), "phone key is its usage index");

        // The bounds the descriptors declare (Logical Maximum 6 and 17).
        expect(isKnownMediaKey(6) && !isKnownMediaKey(7), "media keys stop at navGuidance");
        expect(isKnownTelephonyKey(17) && !isKnownTelephonyKey(18), "phone keys stop at delete");
    }

    // The command wrapper the event channel actually carries.
    {
        const Bytes body = sendReportCommand(kKnobUid, knobReport({.select = true}));
        const auto decoded = plist::decodeBinary(body);
        expect(decoded.has_value() && decoded->isDict(), "report command is a binary plist dict");
        if (decoded && decoded->isDict())
        {
            expect(decoded->find("type")->asString() == "hidSendReport", "command type");
            expect(decoded->find("uuid")->asString() == "2a2a2a2b", "command names the device");
            expect(decoded->find("hidReport")->asData() == knobReport({.select = true}),
                   "command carries the report verbatim");
        }
    }

    if (failures == 0)
    {
        SPDLOG_INFO("hid tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
