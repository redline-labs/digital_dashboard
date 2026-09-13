// The display record: what redline-display-setup writes, and what gets read.
//
// The main fixture is the file a real boot wrote on the LattePanda, verbatim --
// including the trailing space on both sensor lines, which a split on a single
// space would turn into an empty path.

#include "display_backlight/display_record.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

using namespace display_backlight;

// /run/redline/displays/primary on 10.0.0.91, 2026-09-12.
const char* kBoardRecord =
    "[display]\n"
    "role=primary\n"
    "profile=rivian-ic-la123wf9\n"
    "profile_path=/usr/share/redline/displays/rivian-ic-la123wf9.conf\n"
    "name=Rivian Gen1 IC, LG LA123WF9-SL07, 1920x720\n"
    "card=card0\n"
    "connector=HDMI-A-2\n"
    "native_mode=1920x720\n"
    "size_mm=292x110\n"
    "[backlight]\n"
    "type=lp8863\n"
    "device=/sys/class/backlight/lp8863\n"
    "max=65535\n"
    "[sensors]\n"
    "ambient_light=/sys/bus/iio/devices/iio:device0 /sys/bus/iio/devices/iio:device1 \n"
    "temperature=/sys/class/hwmon/hwmon1 /sys/class/hwmon/hwmon2 \n"
    "[link]\n"
    "i2c_bus=11\n"
    "ser_addr=0x0C\n"
    "ser_ident=_UB949\n"
    "ser_sysfs=/sys/bus/i2c/devices/11-000c\n"
    "des_addr=0x32\n"
    "des_ident=_UB948\n"
    "eeprom=123N2N4U00GEHN0.5\n";

void testTheBoardsRecord()
{
    const auto record = parseDisplayRecord(kBoardRecord);
    check(record.has_value(), "the board's record parses");
    if (!record)
    {
        return;
    }

    check(record->role == "primary", "role");
    check(record->profile == "rivian-ic-la123wf9", "profile");
    check(record->name == "Rivian Gen1 IC, LG LA123WF9-SL07, 1920x720", "a name with commas and spaces survives");
    check(record->connector == "HDMI-A-2", "connector");
    check(record->nativeMode == "1920x720", "native mode");
    check(record->backlightType == "lp8863", "backlight type");
    check(record->backlightDevice == "/sys/class/backlight/lp8863", "backlight device");
    check(record->backlightMax == std::optional<uint32_t>(65535), "backlight max");
    check(!record->backlightViaSerializer(), "a kernel-owned backlight is not the serializer");

    check(record->ambientLightSensors.size() == 2, "both light sensors, and no empty third from the trailing space");
    check(record->ambientLightSensors.size() == 2 &&
              record->ambientLightSensors[1] == "/sys/bus/iio/devices/iio:device1",
          "the second light sensor's path is exact");
    check(record->temperatureSensors.size() == 2 && record->temperatureSensors[0] == "/sys/class/hwmon/hwmon1",
          "both temperature sensors");
    check(record->warnings.empty(), "the board's record has nothing to warn about");
}

// The hand-off document's example, which annotates a value with a comment.
void testInlineCommentsAreStripped()
{
    const auto record = parseDisplayRecord(
        "[display]\n"
        "role=primary\n"
        "[backlight]\n"
        "device=serializer        # or a /sys/class/backlight path when a driver owns it\n");
    check(record.has_value() && record->backlightDevice == "serializer",
          "a trailing comment is not part of the value");
    check(record.has_value() && record->backlightViaSerializer(), "the serializer case is recognised");
}

void testAHashInsideAValueIsKept()
{
    const auto record = parseDisplayRecord("[display]\nrole=primary\nname=Panel#2 rev;B\n");
    check(record.has_value() && record->name == "Panel#2 rev;B",
          "'#' and ';' with no whitespace before them are part of the value");
}

void testJunkLinesAreSkippedAndReported()
{
    const auto record = parseDisplayRecord(
        "orphan=1\n"
        "[display]\n"
        "role=secondary\n"
        "this line has no equals sign\n"
        "[]\n"
        "connector=HDMI-A-1\n");
    check(record.has_value(), "a record with junk in it still parses");
    if (record)
    {
        check(record->role == "secondary", "the good keys around the junk are read");
        // orphan=1, the line with no '=', the empty header, and the key under it.
        check(record->warnings.size() == 4,
              "each junk line is reported, got " + std::to_string(record->warnings.size()));
        check(record->connector.empty(), "a key after a malformed header is not filed under the previous section");
    }
}

void testWhatIsNotARecordIsRefused()
{
    check(!parseDisplayRecord("").has_value(), "an empty file is not a record");
    check(!parseDisplayRecord("[display]\nprofile=x\n").has_value(), "a record with no role is refused");
    check(!parseDisplayRecord("[display]\nrole=primary\n[backlight]\nmax=lots\n").has_value(),
          "an unreadable backlight max is refused rather than read as zero");
}

void testLoadingDistinguishesMissingFromBroken()
{
    const auto dir = std::filesystem::temp_directory_path() / "display_backlight_test_record";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const auto missing = loadDisplayRecord(dir / "secondary");
    check(!missing.has_value() && missing.error().kind == RecordError::Kind::missing,
          "no file is `missing`: the normal state of an empty slot");

    std::ofstream(dir / "broken") << "not a record\n";
    const auto broken = loadDisplayRecord(dir / "broken");
    check(!broken.has_value() && broken.error().kind == RecordError::Kind::malformed, "junk is `malformed`");

    std::ofstream(dir / "primary") << kBoardRecord;
    check(loadDisplayRecord(dir / "primary").has_value(), "the board's record loads from a file");

    std::filesystem::remove_all(dir);
}

}  // namespace

int main()
{
    testTheBoardsRecord();
    testInlineCommentsAreStripped();
    testAHashInsideAValueIsKept();
    testJunkLinesAreSkippedAndReported();
    testWhatIsNotARecordIsRefused();
    testLoadingDistinguishesMissingFromBroken();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
