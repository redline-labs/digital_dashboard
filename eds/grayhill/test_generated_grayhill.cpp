// SPDX-License-Identifier: GPL-3.0-or-later
//
// The code generated from the Grayhill EDS, checked against the manual.
//
// This is the test that would have caught the state the library was in: the
// generator's COB-ID lookups had been failing silently for as long as the
// parser had been unable to read `$NODEID+0x40000180`, so the values in the
// generated header were the DS401 fallbacks rather than anything derived. They
// were also, by coincidence, correct -- which is why nothing noticed. Asserting
// the numbers here does not distinguish derived from hardcoded on its own; the
// generator refusing to emit a fallback is what does that, and this test is
// what proves the refusal did not simply break the build's numbers.
//
// The expected values come from Grayhill 3KUM1331-1 rev D Table 1, not from the
// EDS, so a change to the EDS that contradicts the manual fails here.

#include "canopen_grayhill_helpers.h"
#include "canopen_grayhill_node.h"

#include <spdlog/spdlog.h>

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

// Grayhill's factory default node ID.
constexpr uint8_t kNode = 0x0A;

void test_device_identity()
{
    check(canopen_grayhill::VENDOR_ID == 0x0307, "vendor ID is 0x0307");
    check(canopen_grayhill::PRODUCT_CODE == 0x334B, "product code is 0x334B (\"3K\")");
    check(canopen_grayhill::LSS_SUPPORTED, "the device supports LSS");
}

void test_cobids()
{
    // Table 1: RPDO1 0x20A, RPDO2 0x30A, TPDO1 0x18A at the factory node ID.
    check(canopen_grayhill::cobid_rpdo1(kNode) == 0x20A, "RPDO1 (indicators) is 0x20A at node 10");
    check(canopen_grayhill::cobid_rpdo2(kNode) == 0x30A, "RPDO2 (brightness) is 0x30A at node 10");
    check(canopen_grayhill::cobid_tpdo1(kNode) == 0x18A, "TPDO1 (buttons) is 0x18A at node 10");

    // A different node ID must move all three, which is the property a
    // hardcoded COB-ID would still satisfy but a hardcoded *frame* would not.
    check(canopen_grayhill::cobid_tpdo1(0x0B) == 0x18B, "TPDO1 follows the node ID");
}

void test_frame_lengths()
{
    // Derived from the mappings: 0x1A00 maps three bytes of 0x6000, 0x1600
    // eight bytes of 0x6200, 0x1601 two 16-bit values of 0x6411.
    check(canopen_grayhill::TPDO1_LENGTH == 3, "TPDO1 is three bytes");
    check(canopen_grayhill::RPDO1_LENGTH == 8, "RPDO1 is eight bytes");
    check(canopen_grayhill::RPDO2_LENGTH == 4, "RPDO2 is four bytes");
}

void test_button_decode()
{
    // Buttons 1..24, one bit each, in three bytes.
    helpers::CanFrame frame {};
    frame.id = canopen_grayhill::cobid_tpdo1(kNode);
    frame.len = 3;
    frame.data[0] = 0x01; // button 1
    frame.data[1] = 0x80; // button 16
    frame.data[2] = 0x04; // button 19

    auto buttons = canopen_grayhill::unpack_tpdo1(frame);
    check(buttons.digital_input_buttons_1_through_8 == 0x01, "button 1 decodes from byte 0");
    check(buttons.digital_input_buttons_9_through_16 == 0x80, "button 16 decodes from byte 1");
    check(buttons.digital_input_buttons_17_through_24 == 0x04, "button 19 decodes from byte 2");
}

void test_indicator_pack()
{
    canopen_grayhill::Rpdo1 indicators {};
    // The manual's layout rule: three indicators per button, always addressed
    // whether the LED is fitted or not, LSB first.
    indicators.digital_output_indicators_1_through_8 = 0b0000'0101;
    indicators.digital_output_indicators_57_through_64 = 0xFF;

    auto frame = canopen_grayhill::pack_rpdo1(indicators, kNode);
    check(frame.id == 0x20A, "indicator frame goes to RPDO1");
    check(frame.len == 8, "indicator frame is a full eight bytes");
    check(frame.data[0] == 0b0000'0101, "the first indicator byte lands in byte 0");
    check(frame.data[7] == 0xFF, "the eighth lands in byte 7");

    auto roundTrip = canopen_grayhill::unpack_rpdo1(frame);
    check(roundTrip.digital_output_indicators_1_through_8
              == indicators.digital_output_indicators_1_through_8,
          "indicators survive a round trip");
    check(roundTrip.digital_output_indicators_57_through_64
              == indicators.digital_output_indicators_57_through_64,
          "including the last byte");
}

void test_brightness_pack()
{
    // Both channels travel in one frame. The runtime node used to send this
    // frame with one channel zeroed, which blanked whichever channel the
    // caller was not setting -- and for the indicator channel zero is below
    // its documented minimum of 1.
    canopen_grayhill::Rpdo2 brightness {};
    brightness.analog_output_indicator_brightness = 200;
    brightness.analog_output_backlight_brightness = 128;

    auto frame = canopen_grayhill::pack_rpdo2(brightness, kNode);
    check(frame.id == 0x30A, "brightness frame goes to RPDO2");
    check(frame.len == 4, "brightness frame is four bytes");
    check(frame.data[0] == 200 && frame.data[1] == 0, "indicator brightness is little-endian");
    check(frame.data[2] == 128 && frame.data[3] == 0, "backlight brightness follows it");

    auto roundTrip = canopen_grayhill::unpack_rpdo2(frame);
    check(roundTrip.analog_output_indicator_brightness == 200
              && roundTrip.analog_output_backlight_brightness == 128,
          "both channels survive a round trip");
}

void test_node_dispatch()
{
    canopen_grayhill::node device(kNode);

    int calls = 0;
    uint8_t seen = 0;
    device.on_tpdo1(
        [&](const canopen_grayhill::Tpdo1& buttons)
        {
            ++calls;
            seen = buttons.digital_input_buttons_1_through_8;
        });

    helpers::CanFrame buttons {};
    buttons.id = 0x18A;
    buttons.len = 3;
    buttons.data[0] = 0x42;
    check(device.handle_frame(buttons), "a TPDO1 frame is consumed");
    check(calls == 1 && seen == 0x42, "and reaches the callback decoded");

    // Another node's TPDO1. Left alone so the caller can route it elsewhere.
    helpers::CanFrame other {};
    other.id = 0x18B;
    other.len = 3;
    check(!device.handle_frame(other), "another node's TPDO1 is not consumed");
    check(calls == 1, "and does not reach the callback");

    // A heartbeat. The generated class handles PDOs only.
    helpers::CanFrame heartbeat {};
    heartbeat.id = 0x700 + kNode;
    heartbeat.len = 1;
    check(!device.handle_frame(heartbeat), "a heartbeat is left for the CANopen stack");

    // A truncated TPDO1: decoding it would invent two bytes of button state.
    helpers::CanFrame truncated {};
    truncated.id = 0x18A;
    truncated.len = 1;
    check(!device.handle_frame(truncated), "a short TPDO1 is dropped rather than decoded");
    check(calls == 1, "and does not reach the callback");
}

void test_make_frames()
{
    canopen_grayhill::node device(kNode);
    check(device.node_id() == kNode, "the node remembers its id");

    canopen_grayhill::Rpdo2 brightness {};
    brightness.analog_output_backlight_brightness = 64;
    auto frame = device.make_rpdo2(brightness);
    check(frame.id == 0x30A, "make_rpdo2 addresses the configured node");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_device_identity();
    test_cobids();
    test_frame_lengths();
    test_button_decode();
    test_indicator_pack();
    test_brightness_pack();
    test_node_dispatch();
    test_make_frames();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all generated-code checks passed");
    return 0;
}
