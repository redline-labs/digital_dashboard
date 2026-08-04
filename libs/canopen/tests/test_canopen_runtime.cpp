// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CANopen runtime -- SDO, NMT, LSS -- against a keypad built out of the
// shipped EDS.
//
// The point of this file is that the reconfiguration described in the plan can
// be shown to work, in full, before any hardware exists. Everything a real
// session would hit is reachable here: an abort from writing a read-only
// object, a value refused for being outside its limits, a device that does not
// answer, a store that persists across a reset and a write that does not, and
// an LSS reconfiguration after which the device is only reachable at a new
// address and a new bit rate.

#include "canopen/eds_parser.h"
#include "canopen/lss.h"
#include "canopen/nmt.h"
#include "canopen/sdo.h"
#include "canopen/stub_device.h"
#include "canopen/virtual_bus.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>
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

constexpr uint8_t kNode = 0x0A;

canopen::ObjectDictionary load_grayhill()
{
    const std::string path = std::string(CANOPEN_EDS_DIR) + "/grayhill/DS401_3K_C.eds";
    std::ifstream in(path);
    std::ostringstream buffer;
    buffer << in.rdbuf();

    auto result = canopen::parse_eds(buffer.str());
    if (!result.ok())
    {
        SPDLOG_ERROR("the shipped EDS did not parse; every check below is meaningless");
        ++failures;
    }
    return std::move(result.od);
}

// Everything a test needs, wired the way the reconfiguration tool wires it.
struct Rig
{
    canopen::VirtualBus bus { 250 };
    canopen::StubDevice keypad;
    canopen::NmtMaster nmt { bus };
    canopen::SdoClient sdo { bus, kNode };

    explicit Rig(bool singleNodeBus = true)
        : keypad(bus, load_grayhill(), kNode, canopen::LssBitrate::Rate250k)
        , lss(bus, singleNodeBus)
    {
    }

    canopen::LssMaster lss;
};

// ============================================================================
// SDO
// ============================================================================

void test_upload_widths()
{
    Rig rig;

    // The two objects PDM Manager's gate reads. Their widths are the entire
    // question: its SDO reader compares the response command byte for exact
    // equality, so a device that served 0x2010:02 as one byte would be
    // rejected with "Invalid response from Keypad" whatever its value.
    auto backlight = rig.sdo.upload(0x2010, 2);
    check(backlight.has_value(), "0x2010:02 reads back");
    if (backlight.has_value())
    {
        check(backlight->size() == 2, "0x2010:02 is served as two bytes (SDO cs 0x4B)");
        check(backlight->as_uint() == 0xFF, "and reads its factory value of 0xFF");
    }

    auto txType = rig.sdo.upload(0x1800, 2);
    check(txType.has_value(), "0x1800:02 reads back");
    if (txType.has_value())
    {
        check(txType->size() == 1, "0x1800:02 is served as one byte (SDO cs 0x4F)");
        check(txType->as_uint() == 0xFF, "and reads its factory value of 0xFF");
    }

    // The COB-IDs, which the EDS writes as $NODEID expressions. Reading 0x18A
    // here means the whole chain -- expression parsed, resolved against the
    // node ID, seeded into the device, served over SDO -- is intact.
    auto cobid = rig.sdo.upload_u32(0x1800, 1);
    check(cobid.has_value() && (*cobid & 0x1FFFFFFF) == 0x18A,
          "TPDO1's COB-ID reads back as 0x18A at node 10");
}

void test_identity()
{
    Rig rig;

    auto vendor = rig.sdo.upload_u32(0x1018, 1);
    check(vendor.has_value() && *vendor == 0x0307, "vendor ID reads 0x0307");

    auto product = rig.sdo.upload_u32(0x1018, 2);
    check(product.has_value() && *product == 0x334B, "product code reads 0x334B");

    auto deviceType = rig.sdo.upload_u32(0x1000, 0);
    check(deviceType.has_value() && *deviceType == 0x000B0191,
          "device type reads 0x000B0191 -- a VAR object served from its own section");
}

void test_segmented_upload()
{
    Rig rig;

    // The only reason segmented transfer exists in this library: the
    // VISIBLE_STRING identity objects are longer than four bytes.
    auto name = rig.sdo.upload_string(0x1008, 0);
    check(name.has_value(), "0x1008 uploads");
    check(name.has_value() && *name == "manufacturer",
          "and the segmented transfer reassembles it exactly");
}

void test_abort_on_read_only()
{
    Rig rig;

    // 0x1000 is `ro` in the file.
    auto result = rig.sdo.download_u32(0x1000, 0, 1);
    check(!result.has_value(), "writing a read-only object fails");
    if (!result.has_value())
    {
        check(result.error().kind == canopen::SdoError::Kind::Abort, "and fails as an abort");
        check(result.error().abortCode
                  == static_cast<uint32_t>(canopen::SdoAbortCode::WriteOfReadOnly),
              "with the read-only abort code");
        check(result.error().message.find("read-only") != std::string::npos,
              "and a message that says so");
    }
}

void test_abort_on_limits()
{
    Rig rig;

    // The manual gives 0x2010:02 a range of 0x40..0xFF, and the corrected EDS
    // now says so too. Below that, the device refuses.
    auto tooLow = rig.sdo.download_u16(0x2010, 2, 0x10);
    check(!tooLow.has_value(), "a backlight scalar below 0x40 is refused");
    check(!tooLow.has_value()
              && tooLow.error().abortCode
                  == static_cast<uint32_t>(canopen::SdoAbortCode::ValueTooLow),
          "with the value-too-low abort code");

    // And the value the device was already holding is untouched.
    auto value = rig.sdo.upload_u16(0x2010, 2);
    check(value.has_value() && *value == 0xFF, "a refused write does not change the value");
}

void test_abort_on_wrong_width()
{
    Rig rig;

    // Writing the two-byte 0x2010:02 as one byte. A device that accepted this
    // would let the tool ship a bug that only appears against real firmware.
    auto narrow = rig.sdo.download_u8(0x2010, 2, 0xFE);
    check(!narrow.has_value(), "a one-byte write to a two-byte object is refused");
    check(!narrow.has_value()
              && narrow.error().abortCode
                  == static_cast<uint32_t>(canopen::SdoAbortCode::LengthTooLow),
          "with a length abort");
}

void test_missing_objects()
{
    Rig rig;

    auto absent = rig.sdo.upload_u32(0x5555, 0);
    check(!absent.has_value()
              && absent.error().abortCode
                  == static_cast<uint32_t>(canopen::SdoAbortCode::ObjectDoesNotExist),
          "an absent object aborts with 'object does not exist'");

    auto absentSub = rig.sdo.upload_u32(0x1018, 9);
    check(!absentSub.has_value()
              && absentSub.error().abortCode
                  == static_cast<uint32_t>(canopen::SdoAbortCode::SubIndexDoesNotExist),
          "an absent sub-index aborts with 'sub-index does not exist'");
}

void test_timeout()
{
    Rig rig;
    rig.keypad.set_present(false);

    auto result = rig.sdo.upload_u32(0x1018, 1);
    check(!result.has_value(), "a device that is not there does not answer");
    check(!result.has_value() && result.error().kind == canopen::SdoError::Kind::Timeout,
          "and the failure is a timeout, not a bad response");

    // A device that is there but slower than the client's patience is the same
    // failure from the client's side, which is worth knowing when choosing a
    // timeout for real hardware.
    Rig slow;
    slow.keypad.set_response_delay(canopen::Duration { 2000 });
    slow.sdo.set_timeout(canopen::Duration { 100 });
    auto late = slow.sdo.upload_u32(0x1018, 1);
    check(!late.has_value() && late.error().kind == canopen::SdoError::Kind::Timeout,
          "so is a device that answers too late");
}

void test_exchange_log()
{
    Rig rig;

    std::vector<std::string> lines;
    rig.sdo.on_exchange([&](const std::string& line) { lines.push_back(line); });

    (void)rig.sdo.upload_u16(0x2010, 2);
    check(lines.size() == 2, "an exchange logs one line out and one line in");
    check(!lines.empty() && lines[0].starts_with("->"), "the request is logged first");
    check(lines.size() > 1 && lines[1].starts_with("<-"), "then the response");
}

// ============================================================================
// NMT
// ============================================================================

void test_reset_and_bootup()
{
    Rig rig;

    check(rig.keypad.reset_count() == 0, "the device starts un-reset");
    check(rig.nmt.reset_and_wait(kNode), "a reset is acknowledged by a boot-up frame");
    check(rig.keypad.reset_count() == 1, "and the device really did reset");
    check(rig.nmt.state(kNode).has_value(), "the boot-up was observed");

    // Waiting for a boot-up that never comes must fail rather than hang.
    rig.keypad.set_present(false);
    check(!rig.nmt.wait_for_bootup(kNode, canopen::Duration { 200 }),
          "waiting for a boot-up from a silent device times out");
}

void test_state_tracking()
{
    Rig rig;

    rig.nmt.command(canopen::NmtCommand::Start, kNode);
    rig.bus.poll(canopen::Duration { 10 });
    check(rig.keypad.state() == canopen::NmtState::Operational, "NMT start reaches the device");

    rig.nmt.command(canopen::NmtCommand::EnterPreOperational, kNode);
    rig.bus.poll(canopen::Duration { 10 });
    check(rig.keypad.state() == canopen::NmtState::PreOperational, "so does pre-operational");

    // Node 0 is a broadcast, and it is the first frame of the LSS sequence.
    rig.nmt.command(canopen::NmtCommand::Stop, 0);
    rig.bus.poll(canopen::Duration { 10 });
    check(rig.keypad.state() == canopen::NmtState::Stopped, "node 0 addresses every node");
}

// ============================================================================
// The reconfiguration itself
// ============================================================================

void test_motec_compatibility_sequence()
{
    Rig rig;

    // The minimal recipe: put the device in pre-operational, write the two
    // values PDM Manager's OpVerifyKeypad demands, save, reset, read back.
    rig.nmt.command(canopen::NmtCommand::EnterPreOperational, kNode);
    rig.bus.poll(canopen::Duration { 10 });

    check(rig.sdo.download_u8(0x1800, 2, 0xFE).has_value(),
          "TPDO1 transmission type accepts 0xFE");
    check(rig.sdo.download_u16(0x2010, 2, 0xFE).has_value(),
          "backlight brightness scalar accepts 0x00FE");

    // Before the save, nothing is in non-volatile memory.
    check(!rig.keypad.has_stored_parameters(), "nothing has been stored yet");

    check(rig.sdo.store_parameters().has_value(), "0x1010:01 <- \"save\" succeeds");
    check(rig.keypad.has_stored_parameters(), "and the device really stored something");

    check(rig.nmt.reset_and_wait(kNode), "the device resets and announces it");

    auto txType = rig.sdo.upload_u8(0x1800, 2);
    auto backlight = rig.sdo.upload_u16(0x2010, 2);
    check(txType.has_value() && *txType == 0xFE, "0x1800:02 is still 0xFE after the reset");
    check(backlight.has_value() && *backlight == 0xFE, "0x2010:02 is still 0xFE after the reset");
}

void test_unsaved_changes_do_not_survive()
{
    Rig rig;

    check(rig.sdo.download_u8(0x1800, 2, 0xFE).has_value(), "the write succeeds");
    check(rig.nmt.reset_and_wait(kNode), "the device resets");

    auto txType = rig.sdo.upload_u8(0x1800, 2);
    check(txType.has_value() && *txType == 0xFF,
          "a write that was never stored is back to its factory value -- which is what "
          "makes the Store step load-bearing rather than decorative");
}

void test_two_step_cobid_write()
{
    Rig rig;

    // CiA requires a COB-ID to be invalidated before it is changed. Both
    // halves are ordinary SDO writes; what matters is that the device ends up
    // holding the new value with the valid bit set.
    check(rig.sdo.download_u32(0x1800, 1, 0x80000000).has_value(), "the PDO is invalidated");
    check(rig.sdo.download_u32(0x1800, 1, 0x40000000 | 0x18B).has_value(),
          "then given its new COB-ID with RTR disabled");

    auto cobid = rig.sdo.upload_u32(0x1800, 1);
    check(cobid.has_value() && (*cobid & 0x1FFFFFFF) == 0x18B, "and holds it");
    check(cobid.has_value() && (*cobid & 0x40000000) != 0, "with the RTR-not-supported bit set");
}

// ============================================================================
// LSS
// ============================================================================

void test_lss_refuses_without_single_node_assertion()
{
    Rig rig(false);

    auto result = rig.lss.enter_configuration();
    check(!result.has_value(), "LSS switch-state-global is refused on an unasserted bus");
    check(!result.has_value() && result.error().kind == canopen::LssError::Kind::Refused,
          "and the refusal is local, not a device response");
    check(rig.bus.sent().empty(), "nothing was put on the wire");
}

void test_lss_ignored_outside_configuration_mode()
{
    Rig rig;

    // A device not in configuration mode ignores LSS entirely -- silently,
    // like real hardware. A tool that forgets the switch sees a timeout.
    rig.lss.set_timeout(canopen::Duration { 100 });
    auto result = rig.lss.configure_node_id(0x0B);
    check(!result.has_value() && result.error().kind == canopen::LssError::Kind::Timeout,
          "configuring a node ID without entering configuration mode times out");
}

void test_lss_reconfiguration()
{
    Rig rig;

    check(rig.lss.enter_configuration().has_value(), "the bus enters LSS configuration mode");
    check(rig.lss.configure_node_id(0x0B).has_value(), "the new node ID is accepted");
    check(rig.lss.configure_bitrate(canopen::LssBitrate::Rate1000k).has_value(),
          "so is the new bit rate");

    // Nothing has changed yet: LSS writes take effect on the next reset.
    check(rig.keypad.node_id() == kNode, "the device is still at its old node ID");

    check(rig.lss.store_configuration().has_value(), "the configuration is stored");
    check(rig.lss.exit_configuration().has_value(), "and the bus leaves configuration mode");

    rig.nmt.command(canopen::NmtCommand::ResetNode, kNode);
    rig.bus.poll(canopen::Duration { 100 });

    check(rig.keypad.node_id() == 0x0B, "after the reset the device is at its new node ID");
    check(rig.keypad.bitrate() == canopen::LssBitrate::Rate1000k, "and its new bit rate");

    // The client is still speaking 250 kbit/s, so the device cannot hear it.
    // This is exactly the state a mis-ordered reconfiguration leaves you in.
    rig.sdo.set_node_id(0x0B);
    rig.sdo.set_timeout(canopen::Duration { 100 });
    auto stale = rig.sdo.upload_u32(0x1018, 1);
    check(!stale.has_value(), "the device is unreachable at the old bit rate");

    rig.bus.set_bitrate_kbps(1000);
    auto found = rig.sdo.upload_u32(0x1018, 1);
    check(found.has_value() && *found == 0x0307,
          "and reachable again once the client follows it to 1 Mbit/s");
}

void test_lss_refuses_unsupported_bitrate()
{
    Rig rig;

    check(rig.lss.enter_configuration().has_value(), "configuration mode entered");
    // The EDS declares 10 kbit/s unsupported, and the manual agrees.
    auto result = rig.lss.configure_bitrate(canopen::LssBitrate::Rate10k);
    check(!result.has_value(), "the device refuses a bit rate its EDS says it does not support");
    check(!result.has_value() && result.error().kind == canopen::LssError::Kind::Rejected,
          "and refuses it rather than timing out");
}

void test_lss_rejects_bad_node_id()
{
    Rig rig;

    auto result = rig.lss.configure_node_id(0);
    check(!result.has_value() && result.error().kind == canopen::LssError::Kind::Refused,
          "node ID 0 is refused locally rather than sent");
    check(rig.bus.sent().empty(), "nothing was put on the wire");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_upload_widths();
    test_identity();
    test_segmented_upload();
    test_abort_on_read_only();
    test_abort_on_limits();
    test_abort_on_wrong_width();
    test_missing_objects();
    test_timeout();
    test_exchange_log();

    test_reset_and_bootup();
    test_state_tracking();

    test_motec_compatibility_sequence();
    test_unsaved_changes_do_not_survive();
    test_two_step_cobid_write();

    test_lss_refuses_without_single_node_assertion();
    test_lss_ignored_outside_configuration_mode();
    test_lss_reconfiguration();
    test_lss_refuses_unsupported_bitrate();
    test_lss_rejects_bad_node_id();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all CANopen runtime checks passed");
    return 0;
}
