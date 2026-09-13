#include "display_backlight/sysfs.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

namespace display_backlight
{
namespace fs = std::filesystem;

namespace
{

std::optional<uint32_t> readNumber(const fs::path& path)
{
    const auto text = readAttribute(path);
    return text ? parseNumber(*text) : std::nullopt;
}

}  // namespace

std::optional<std::string> readAttribute(const fs::path& path)
{
    std::ifstream in(path);
    if (!in)
    {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (in.bad())
    {
        return std::nullopt;
    }

    std::string text = buffer.str();
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    {
        text.pop_back();
    }
    return text;
}

std::optional<uint32_t> parseNumber(std::string_view text)
{
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        base = 16;
        text.remove_prefix(2);
    }
    if (text.empty())
    {
        return std::nullopt;
    }

    uint32_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (ec != std::errc{} || end != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
}

std::optional<double> parseDecimal(std::string_view text)
{
    if (text.empty() || std::isspace(static_cast<unsigned char>(text.front())))
    {
        return std::nullopt;
    }

    // strtod rather than from_chars: libc++ on the dev hosts has no floating
    // point from_chars. It needs a terminated string, hence the copy.
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(copy.c_str(), &end);
    if (errno != 0 || end != copy.c_str() + copy.size() || !std::isfinite(value))
    {
        return std::nullopt;
    }
    return value;
}

std::optional<std::vector<uint16_t>> parseFaults(std::string_view text)
{
    std::vector<uint16_t> words;
    std::istringstream in{std::string(text)};
    std::string word;
    while (in >> word)
    {
        const auto value = parseNumber(word);
        if (!value || *value > std::numeric_limits<uint16_t>::max())
        {
            return std::nullopt;
        }
        words.push_back(static_cast<uint16_t>(*value));
    }
    if (words.empty())
    {
        return std::nullopt;
    }
    return words;
}

std::optional<FsmState> parseFsmState(std::string_view text)
{
    const std::size_t space = text.find(' ');
    const auto code = parseNumber(text.substr(0, space));
    if (!code || *code > std::numeric_limits<uint8_t>::max())
    {
        return std::nullopt;
    }

    FsmState state;
    state.code = static_cast<uint8_t>(*code);
    if (space != std::string_view::npos)
    {
        std::string_view name = text.substr(space + 1);
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front())))
        {
            name.remove_prefix(1);
        }
        state.name = std::string(name);
    }
    return state;
}

BacklightStatus readBacklight(const fs::path& device)
{
    BacklightStatus status;
    status.brightness = readNumber(device / "brightness");
    status.actualBrightness = readNumber(device / "actual_brightness");
    status.maxBrightness = readNumber(device / "max_brightness");
    status.blPower = readNumber(device / "bl_power");
    status.type = readAttribute(device / "type");

    const fs::path driver = device / "device";
    if (const auto faults = readAttribute(driver / "faults"))
    {
        status.faults = parseFaults(*faults);
    }
    if (const auto state = readAttribute(driver / "fsm_state"))
    {
        status.fsmState = parseFsmState(*state);
    }
    status.ledCurrent = readNumber(driver / "led_current");
    status.pwmOutput = readNumber(driver / "pwm_output");
    status.boost = readNumber(driver / "boost");
    return status;
}

std::expected<void, std::string> writeBrightness(const fs::path& device, uint32_t raw)
{
    const fs::path path = device / "brightness";
    std::ofstream out(path);
    if (!out)
    {
        return std::unexpected("cannot open " + path.string() + ": " + std::strerror(errno));
    }

    // A sysfs store callback rejects a bad value at the write, which a buffered
    // stream only reports once it flushes.
    out << raw << '\n';
    out.flush();
    if (!out)
    {
        return std::unexpected("writing " + std::to_string(raw) + " to " + path.string() + " failed: " +
                               std::strerror(errno));
    }
    return {};
}

uint32_t percentToRaw(double percent, uint32_t max)
{
    if (!std::isfinite(percent))
    {
        return 0;
    }
    const double clamped = std::clamp(percent, 0.0, 100.0);
    return static_cast<uint32_t>(std::llround(clamped / 100.0 * static_cast<double>(max)));
}

double rawToPercent(uint32_t raw, uint32_t max)
{
    if (max == 0)
    {
        return 0.0;
    }
    return 100.0 * static_cast<double>(std::min(raw, max)) / static_cast<double>(max);
}

LightReading readLightSensor(const fs::path& iio)
{
    LightReading reading;
    reading.path = iio.string();
    reading.name = readAttribute(iio / "name").value_or("");
    if (const auto text = readAttribute(iio / "in_illuminance_input"))
    {
        reading.lux = parseDecimal(*text);
    }
    return reading;
}

std::vector<TemperatureReading> readTemperatures(const fs::path& hwmon)
{
    const std::string name = readAttribute(hwmon / "name").value_or("");

    // temp<n>_input, sorted by n rather than by directory order, so temp10 does
    // not come before temp2 and the list is stable from one read to the next.
    std::vector<std::pair<uint32_t, std::string>> channels;
    std::error_code ec;
    for (fs::directory_iterator it(hwmon, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string file = it->path().filename().string();
        constexpr std::string_view kPrefix = "temp";
        constexpr std::string_view kSuffix = "_input";
        if (file.size() <= kPrefix.size() + kSuffix.size() || file.rfind(kPrefix, 0) != 0 ||
            file.compare(file.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
        {
            continue;
        }
        const std::string channel = file.substr(0, file.size() - kSuffix.size());
        if (const auto n = parseNumber(std::string_view(channel).substr(kPrefix.size())))
        {
            channels.emplace_back(*n, channel);
        }
    }
    std::sort(channels.begin(), channels.end());

    std::vector<TemperatureReading> readings;
    for (const auto& [n, channel] : channels)
    {
        TemperatureReading reading;
        reading.path = hwmon.string();
        reading.channel = channel;
        reading.name = name;
        reading.label = readAttribute(hwmon / (channel + "_label")).value_or("");
        if (const auto text = readAttribute(hwmon / (channel + "_input")))
        {
            // Millidegrees, as a signed integer: a panel in a cold car reads
            // below zero, which parseNumber would refuse.
            if (const auto millidegrees = parseDecimal(*text))
            {
                reading.celsius = *millidegrees / 1000.0;
            }
        }
        readings.push_back(std::move(reading));
    }

    if (readings.empty())
    {
        TemperatureReading missing;
        missing.path = hwmon.string();
        missing.name = name;
        readings.push_back(std::move(missing));
    }
    return readings;
}

}  // namespace display_backlight
