// The sysfs readers, against a fake tree laid out as the LattePanda's kernel lays
// out the real one, with the values it held on 2026-09-12.

#include "display_backlight/sysfs.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{

namespace fs = std::filesystem;
using namespace display_backlight;

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

bool near(double a, double b)
{
    return std::fabs(a - b) < 1e-6;
}

void put(const fs::path& path, const std::string& contents)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path) << contents << '\n';
}

struct FakeTree
{
    fs::path root = fs::temp_directory_path() / "display_backlight_test_sysfs";
    fs::path backlight = root / "class/backlight/lp8863";
    fs::path light0 = root / "bus/iio/devices/iio:device0";
    fs::path light1 = root / "bus/iio/devices/iio:device1";
    fs::path coretemp = root / "class/hwmon/hwmon0";
    fs::path tmp1075 = root / "class/hwmon/hwmon1";

    FakeTree()
    {
        fs::remove_all(root);

        put(backlight / "brightness", "24576");
        put(backlight / "actual_brightness", "24576");
        put(backlight / "max_brightness", "65535");
        put(backlight / "bl_power", "0");
        put(backlight / "type", "raw");
        put(backlight / "device/faults", "0x0000 0x0000 0x0800");
        put(backlight / "device/fsm_state", "0xd NORMAL");
        put(backlight / "device/led_current", "0x0fff");
        put(backlight / "device/pwm_output", "0x6000");
        put(backlight / "device/boost", "0x0549");

        put(light0 / "name", "opt3001");
        put(light0 / "in_illuminance_input", "3.420000");
        put(light0 / "in_illuminance_integration_time", "0.800000");
        put(light1 / "name", "opt3001");
        put(light1 / "in_illuminance_input", "2.120000");

        put(coretemp / "name", "coretemp");
        put(coretemp / "temp1_input", "40000");
        put(coretemp / "temp1_label", "Package id 0");
        put(coretemp / "temp2_input", "41000");
        put(coretemp / "temp2_label", "Core 0");
        put(coretemp / "temp10_input", "-5500");

        put(tmp1075 / "name", "tmp1075");
        put(tmp1075 / "temp1_input", "45187");
        put(tmp1075 / "temp1_max", "80000");
    }

    ~FakeTree() { fs::remove_all(root); }
};

void testTheLp8863AsTheBoardHasIt()
{
    const FakeTree tree;
    const BacklightStatus status = readBacklight(tree.backlight);

    check(status.brightness == std::optional<uint32_t>(24576), "brightness");
    check(status.actualBrightness == std::optional<uint32_t>(24576), "actual_brightness");
    check(status.maxBrightness == std::optional<uint32_t>(65535), "max_brightness");
    check(status.blPower == std::optional<uint32_t>(0), "bl_power");
    check(status.type == std::optional<std::string>("raw"), "type, without its newline");

    check(status.faults.has_value() && status.faults->size() == 3 && (*status.faults)[2] == 0x0800,
          "the three fault words");
    check(status.fsmState.has_value() && status.fsmState->code == 0xd && status.fsmState->name == "NORMAL",
          "the state machine's code and name");
    check(status.ledCurrent == std::optional<uint32_t>(0x0fff), "led_current, in hex");
    check(status.pwmOutput == std::optional<uint32_t>(0x6000), "pwm_output, in hex");
    check(status.boost == std::optional<uint32_t>(0x0549), "boost, in hex");
}

// Another module's driver exposes a different subset. Absent must read as absent.
void testMissingAttributesAreAbsentNotZero()
{
    const FakeTree tree;
    fs::remove(tree.backlight / "actual_brightness");
    fs::remove_all(tree.backlight / "device");

    const BacklightStatus status = readBacklight(tree.backlight);
    check(status.brightness.has_value(), "what is there is still read");
    check(!status.actualBrightness.has_value(), "a missing actual_brightness is absent");
    check(!status.faults.has_value() && !status.fsmState.has_value() && !status.boost.has_value(),
          "a driver without the lp8863 attributes reports none of them");

    const BacklightStatus gone = readBacklight(tree.root / "class/backlight/nothing_here");
    check(!gone.brightness.has_value() && !gone.maxBrightness.has_value(), "a missing device reads as nothing");
}

void testTheBrightnessWrite()
{
    const FakeTree tree;
    check(writeBrightness(tree.backlight, 13107).has_value(), "a brightness write succeeds");
    check(readBacklight(tree.backlight).brightness == std::optional<uint32_t>(13107),
          "the written value is what brightness now holds");

    const auto refused = writeBrightness(tree.root / "no/such/device", 1);
    check(!refused.has_value() && refused.error().find("cannot open") != std::string::npos,
          "a write to a device that is not there says why");
}

void testLightSensors()
{
    const FakeTree tree;
    const LightReading reading = readLightSensor(tree.light0);
    check(reading.name == "opt3001", "the IIO device's name");
    check(reading.lux.has_value() && near(*reading.lux, 3.42), "lux from the IIO fixed-point format");

    const LightReading missing = readLightSensor(tree.root / "bus/iio/devices/iio:device9");
    check(!missing.lux.has_value(), "a sensor that is not there has no reading");
}

void testIntegrationTime()
{
    const FakeTree tree;
    const auto written = writeLightIntegrationTime(tree.light0, 0.1);
    check(written.has_value() && *written, "the integration time is written where the attribute exists");
    check(readAttribute(tree.light0 / "in_illuminance_integration_time") == std::optional<std::string>("0.100000"),
          "in the IIO fixed-point format");

    const auto absent = writeLightIntegrationTime(tree.light1, 0.1);
    check(absent.has_value() && !*absent, "a sensor without the attribute is skipped, not an error");
    check(!fs::exists(tree.light1 / "in_illuminance_integration_time"), "and the attribute is not created");

    const auto bad = writeLightIntegrationTime(tree.light0, 0.0);
    check(!bad.has_value(), "zero is refused");
    const auto nan = writeLightIntegrationTime(tree.light0, std::nan(""));
    check(!nan.has_value(), "NaN is refused");
    check(readAttribute(tree.light0 / "in_illuminance_integration_time") == std::optional<std::string>("0.100000"),
          "a refused value leaves the attribute alone");
}

// The node resolves channels once and re-reads only the values.
void testResolvedTemperatureChannels()
{
    const FakeTree tree;
    auto channels = findTemperatureChannels(tree.coretemp);
    check(channels.size() == 3 && !channels[0].celsius.has_value(), "channels are found without values");
    check(channels.size() == 3 && channels[0].label == "Package id 0", "with their labels");

    readTemperatureValues(channels);
    check(channels.size() == 3 && channels[1].celsius && near(*channels[1].celsius, 41.0), "values are read");

    put(tree.coretemp / "temp2_input", "43500");
    fs::remove(tree.coretemp / "temp1_input");
    readTemperatureValues(channels);
    check(channels.size() == 3 && channels[1].celsius && near(*channels[1].celsius, 43.5), "a new value is seen");
    check(channels.size() == 3 && !channels[0].celsius.has_value(),
          "a value that cannot be read clears rather than keeping the last");

    auto gone = findTemperatureChannels(tree.root / "class/hwmon/hwmon7");
    readTemperatureValues(gone);
    check(gone.size() == 1 && gone[0].channel.empty() && !gone[0].celsius.has_value(),
          "a missing sensor stays one entry with no value");
}

void testTemperatures()
{
    const FakeTree tree;

    const auto tmp = readTemperatures(tree.tmp1075);
    check(tmp.size() == 1, "tmp1075 has one channel; temp1_max is not an input");
    check(tmp.size() == 1 && tmp[0].name == "tmp1075" && tmp[0].label.empty(), "no label on a tmp1075");
    check(tmp.size() == 1 && tmp[0].celsius && near(*tmp[0].celsius, 45.187), "millidegrees to degrees");

    const auto core = readTemperatures(tree.coretemp);
    check(core.size() == 3, "every temp<n>_input");
    check(core.size() == 3 && core[0].channel == "temp1" && core[1].channel == "temp2" && core[2].channel == "temp10",
          "channels in numeric order, not directory order");
    check(core.size() == 3 && core[0].label == "Package id 0", "labels where the driver has them");
    check(core.size() == 3 && core[2].celsius && near(*core[2].celsius, -5.5), "below zero reads below zero");

    const auto gone = readTemperatures(tree.root / "class/hwmon/hwmon7");
    check(gone.size() == 1 && !gone[0].celsius.has_value(),
          "a configured sensor that is missing still shows up, as missing");
}

void testParsers()
{
    check(parseNumber("65535") == std::optional<uint32_t>(65535), "decimal");
    check(parseNumber("0x6000") == std::optional<uint32_t>(0x6000), "hex");
    check(!parseNumber("0x").has_value() && !parseNumber("").has_value() && !parseNumber("12a").has_value() &&
              !parseNumber("-1").has_value(),
          "junk is refused");

    check(!parseFaults("0x0000 0x10000").has_value(), "a fault word wider than 16 bits is refused");
    check(!parseFaults("").has_value(), "no fault words is not a fault reading");
    check(!parseFsmState("NORMAL").has_value(), "a state with no code is refused");
    check(parseFsmState("0x2").has_value() && parseFsmState("0x2")->name.empty(), "a code with no name is kept");

    check(!parseDecimal(" 3.4").has_value() && !parseDecimal("3.4lux").has_value() && !parseDecimal("nan").has_value(),
          "a decimal must be the whole string, and finite");
}

void testPercentMapping()
{
    check(percentToRaw(100.0, 65535) == 65535, "100% is max");
    check(percentToRaw(0.0, 65535) == 0, "0% is zero");
    check(percentToRaw(20.0, 65535) == 13107, "20% of 65535");
    check(percentToRaw(250.0, 65535) == 65535 && percentToRaw(-5.0, 65535) == 0, "out of range clamps");
    check(percentToRaw(std::nan(""), 65535) == 0, "NaN does not become a brightness");
    check(near(rawToPercent(24576, 65535), 100.0 * 24576.0 / 65535.0), "the board's brightness is ~37.5%");
    check(rawToPercent(10, 0) == 0.0, "a zero max does not divide by zero");
}

}  // namespace

int main()
{
    testTheLp8863AsTheBoardHasIt();
    testMissingAttributesAreAbsentNotZero();
    testTheBrightnessWrite();
    testLightSensors();
    testTemperatures();
    testIntegrationTime();
    testResolvedTemperatureChannels();
    testParsers();
    testPercentMapping();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
