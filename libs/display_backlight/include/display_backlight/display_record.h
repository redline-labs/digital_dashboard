#ifndef DISPLAY_BACKLIGHT_DISPLAY_RECORD_H_
#define DISPLAY_BACKLIGHT_DISPLAY_RECORD_H_

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The per-boot record redline-display-setup writes for each display it found,
// at /run/redline/displays/<role>. As a real boot wrote it:
//
//   [display]
//   role=primary
//   profile=rivian-ic-la123wf9
//   name=Rivian Gen1 IC, LG LA123WF9-SL07, 1920x720
//   connector=HDMI-A-2
//   native_mode=1920x720
//   [backlight]
//   type=lp8863
//   device=/sys/class/backlight/lp8863
//   max=65535
//   [sensors]
//   ambient_light=/sys/bus/iio/devices/iio:device0 /sys/bus/iio/devices/iio:device1
//   temperature=/sys/class/hwmon/hwmon1 /sys/class/hwmon/hwmon2
//   [link]
//   ...
//
// The file only exists when a module was detected in that slot, so a missing
// record is the normal state of every machine without one -- not an error.
namespace display_backlight
{

using IniSection = std::map<std::string, std::string>;

struct IniParse
{
    std::map<std::string, IniSection> sections;

    // Lines that were not a section, a key=value or a comment. Skipped rather
    // than fatal, and kept so the caller can say so.
    std::vector<std::string> problems;
};

// `[section]` and `key=value` lines. Whitespace around keys and values is
// trimmed. `#` or `;` starts a comment at the start of a line or after
// whitespace -- not anywhere, because a name or a path may contain either.
IniParse parseIni(std::string_view text);

// A space-separated list, as the record writes its sensor paths. Tolerates the
// trailing space the real file ends those lines with.
std::vector<std::string> splitList(std::string_view value);

// What [backlight] device says when no kernel driver owns the backlight and the
// rootfs drives it through the serializer instead. Nothing here can write it.
inline constexpr std::string_view kSerializerDevice = "serializer";

struct DisplayRecord
{
    std::string role;
    std::string profile;
    std::string name;
    std::string connector;
    std::string nativeMode;

    std::string backlightType;
    std::string backlightDevice;
    std::optional<uint32_t> backlightMax;

    std::vector<std::string> ambientLightSensors;
    std::vector<std::string> temperatureSensors;

    // Anything in the file that was skipped.
    std::vector<std::string> warnings;

    bool backlightViaSerializer() const { return backlightDevice == kSerializerDevice; }
};

// Fails only when the text is not a display record at all (no [display] role) or
// a value that matters cannot be read.
std::expected<DisplayRecord, std::string> parseDisplayRecord(std::string_view text);

struct RecordError
{
    enum class Kind
    {
        missing,     // no file: no module was detected in this slot this boot
        unreadable,  // the file is there and could not be opened
        malformed,   // the file is not a display record
    };

    Kind kind;
    std::string message;
};

std::expected<DisplayRecord, RecordError> loadDisplayRecord(const std::filesystem::path& path);

}  // namespace display_backlight

#endif  // DISPLAY_BACKLIGHT_DISPLAY_RECORD_H_
