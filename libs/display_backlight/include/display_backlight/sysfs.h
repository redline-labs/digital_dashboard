#ifndef DISPLAY_BACKLIGHT_SYSFS_H_
#define DISPLAY_BACKLIGHT_SYSFS_H_

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Reading the display module's devices through the standard kernel interfaces
// the rootfs binds them to, and writing the one value this code is allowed to
// write: the backlight brightness.
//
// Every attribute is optional. The set below is what the LattePanda's lp8863_bl
// driver, opt3001 and tmp1075 expose (read off the board on 2026-09-12); another
// module's drivers will expose a different subset, and an attribute that is not
// there is reported as absent rather than as zero.
namespace display_backlight
{

// The contents of a sysfs attribute with the trailing newline and whitespace
// removed, or nullopt if it cannot be read.
std::optional<std::string> readAttribute(const std::filesystem::path& path);

// "65535", "0x6000" or "0xd" -- the whole string, decimal or 0x-prefixed hex.
// The lp8863 driver prints its register attributes in hex and the backlight
// class prints decimal, so both turn up side by side.
std::optional<uint32_t> parseNumber(std::string_view text);

// "3.420000", the IIO fixed-point format. The whole string.
std::optional<double> parseDecimal(std::string_view text);

// The lp8863 `faults` attribute: fault register words, space separated, e.g.
// "0x0000 0x0000 0x0800". nullopt if any word is not a 16-bit number.
std::optional<std::vector<uint16_t>> parseFaults(std::string_view text);

// The lp8863 `fsm_state` attribute: a code and the state's name, e.g. "0xd NORMAL".
struct FsmState
{
    uint8_t code = 0;
    std::string name;

    bool operator==(const FsmState&) const = default;
};
std::optional<FsmState> parseFsmState(std::string_view text);

struct BacklightStatus
{
    // The backlight class attributes.
    std::optional<uint32_t> brightness;         // what was last requested
    std::optional<uint32_t> actualBrightness;   // what the driver reports applied
    std::optional<uint32_t> maxBrightness;
    std::optional<uint32_t> blPower;            // 0 is on (FB_BLANK_UNBLANK)
    std::optional<std::string> type;            // raw | platform | firmware

    // The lp8863 driver's own attributes, under <device>/device/.
    std::optional<std::vector<uint16_t>> faults;
    std::optional<FsmState> fsmState;
    std::optional<uint32_t> ledCurrent;
    std::optional<uint32_t> pwmOutput;
    std::optional<uint32_t> boost;

    bool operator==(const BacklightStatus&) const = default;
};

// `device` is the backlight class directory, e.g. /sys/class/backlight/lp8863.
BacklightStatus readBacklight(const std::filesystem::path& device);

// Writes <device>/brightness. Deliberately the only write here: bl_power and the
// module's enable are redline-display's, which sequences them against valid
// video as the panel requires.
std::expected<void, std::string> writeBrightness(const std::filesystem::path& device, uint32_t raw);

// Percent of `max`, clamped to 0..100. Rounded to nearest.
uint32_t percentToRaw(double percent, uint32_t max);
double rawToPercent(uint32_t raw, uint32_t max);

struct LightReading
{
    std::string path;
    std::string name;             // the IIO device's `name`, e.g. opt3001
    std::optional<double> lux;    // in_illuminance_input
};

// `iio` is the IIO device directory, e.g. /sys/bus/iio/devices/iio:device0.
//
// Can block for a whole conversion: the opt3001 driver starts a single-shot
// conversion on this read and sleeps through it -- about 1 s at its 0.8 s
// integration time, measured on the board. Do not hold a lock across it.
LightReading readLightSensor(const std::filesystem::path& iio);

// Writes in_illuminance_integration_time, in seconds; the opt3001 offers 0.1
// and 0.8 (integration_time_available). Returns false, not an error, when the
// device has no such attribute -- another driver may not.
std::expected<bool, std::string> writeLightIntegrationTime(const std::filesystem::path& iio, double seconds);

struct TemperatureReading
{
    std::string path;              // the hwmon directory
    std::string channel;           // e.g. temp1
    std::string name;              // the hwmon device's `name`, e.g. tmp1075
    std::string label;             // temp<n>_label, where the driver has one
    std::optional<double> celsius; // temp<n>_input is millidegrees
};

// Every temp<n>_input under `hwmon`, in channel order. A directory with none, or
// that is not there, still yields one reading with no value, so a configured
// sensor that has gone missing shows up as missing rather than vanishing.
std::vector<TemperatureReading> readTemperatures(const std::filesystem::path& hwmon);

// readTemperatures() in two halves, so a node lists the directory and reads
// the names and labels once rather than on every pass. The returned readings
// have no value yet; a missing sensor is the one entry with no channel.
std::vector<TemperatureReading> findTemperatureChannels(const std::filesystem::path& hwmon);

// Re-reads the value of each reading findTemperatureChannels() returned.
void readTemperatureValues(std::vector<TemperatureReading>& channels);

}  // namespace display_backlight

#endif  // DISPLAY_BACKLIGHT_SYSFS_H_
