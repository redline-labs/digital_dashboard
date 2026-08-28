// SPDX-License-Identifier: GPL-3.0-or-later
//
// The data services: text messaging, location, registration.
//
// Only TMS has been exercised against a real radio, and this file does not
// pretend otherwise. The LRRP assertions check arithmetic against the
// documented scale and the ARS one checks a nibble; neither is evidence that a
// radio would answer. See the provenance note at the top of nai.h.

#include "mototrbo/nai.h"

#include "golden/hardware_vectors.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdint>
#include <span>
#include <string>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

using namespace mototrbo;

// The XNL ports appear in the community NAI port list too, which is a
// cross-check that the two independent bodies of work describe one radio.
static_assert(nai::kPortXnl == 8002);
static_assert(nai::kPortXnlSecure == 8003);
static_assert(nai::kPortTms == 4007);
static_assert(nai::kPortLrrp == 4001);
static_assert(nai::kPortArs == 4005);

// 0x40000000 is a quarter turn: 45 degrees of latitude, 90 of longitude.
static_assert(nai::lrrp::decode_latitude(0x40000000) == 45.0);
static_assert(nai::lrrp::decode_longitude(0x40000000) == 90.0);
static_assert(nai::lrrp::decode_latitude(-0x40000000) == -45.0);

// The type is the low nibble of byte 2, so the high nibble must not leak in.
static_assert(nai::ars::message_type(golden::hex("0002f0")).value() ==
              nai::ars::MessageType::DeviceRegistration);
static_assert(nai::ars::message_type(golden::hex("0002ff")).value() == nai::ars::MessageType::RegistrationAck);
static_assert(nai::ars::message_type(golden::hex("0002")).error().kind == ErrorKind::Truncated);

// A datagram with no location token yields nothing rather than a coordinate
// scavenged from the first four bytes that happened to be there.
static_assert(!nai::lrrp::find_position(golden::hex("000507220000")).has_value());

// And one with a location token decodes the pair after it. SYNTHETIC: no radio
// we have has answered an LRRP request, so this checks the scale and the token
// scan, not the format. 0x20000000 is an eighth of a turn.
static_assert(nai::lrrp::find_position(golden::hex("0007662000000010000000")).has_value());
static_assert(nai::lrrp::find_position(golden::hex("0007662000000010000000"))->latitudeDeg == 22.5);
static_assert(nai::lrrp::find_position(golden::hex("0007662000000010000000"))->longitudeDeg == 22.5);

void checkTextMessaging()
{
    const Result<std::vector<std::uint8_t>> encoded = nai::tms::encode_text("Hi", 3, true);
    check(encoded.has_value(), "TMS encode");
    if (!encoded.has_value())
    {
        return;
    }

    // 00 <len> A0|40 00 80|3 04 0D 00 0A 00 'H' 00 'i' 00
    check(encoded->size() == 14, "TMS datagram length");
    check((*encoded)[1] == 2 * 2 + 8, "body length is the UTF-16 byte count plus eight");
    check((*encoded)[2] == (0xA0 | 0x40), "acknowledgement requested");
    check((*encoded)[4] == (0x80 | 0x03), "sequence number");
    // UTF-16 LITTLE-endian here, unlike the display broadcasts.
    check((*encoded)[10] == 'H' && (*encoded)[11] == 0x00, "text is UTF-16LE");

    const Result<nai::tms::TextMessage> parsed = nai::tms::parse(*encoded);
    check(parsed.has_value(), "TMS parse");
    if (parsed.has_value())
    {
        check(parsed->type == nai::tms::MessageType::SimpleText, "message type");
        check(parsed->sequence == 3, "sequence round trip");
        check(parsed->text == "Hi", "text round trip");
    }

    // Non-ASCII is refused rather than encoded as a wrong code unit: this
    // encoder widens each byte, which is only correct for ASCII.
    check(!nai::tms::encode_text("caf\xc3\xa9").has_value(), "non-ASCII text is refused");

    // The body length is a single byte, so a long message cannot be framed.
    check(!nai::tms::encode_text(std::string(200, 'x')).has_value(), "over-long text is refused");

    // A runt datagram is an error, not an empty message.
    check(!nai::tms::parse(golden::hex("000c")).has_value(), "a runt TMS datagram is refused");
}

} // namespace

int main()
{
    spdlog::set_pattern("[%^%l%$] %v");

    checkTextMessaging();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }

    SPDLOG_INFO("mototrbo_test_nai passed");
    return 0;
}
