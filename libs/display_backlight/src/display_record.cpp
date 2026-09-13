#include "display_backlight/display_record.h"

#include "display_backlight/sysfs.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace display_backlight
{
namespace
{

bool isSpace(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && isSpace(text.front()))
    {
        text.remove_prefix(1);
    }
    while (!text.empty() && isSpace(text.back()))
    {
        text.remove_suffix(1);
    }
    return text;
}

std::string_view stripComment(std::string_view line)
{
    for (std::size_t i = 0; i < line.size(); ++i)
    {
        if ((line[i] == '#' || line[i] == ';') && (i == 0 || isSpace(line[i - 1])))
        {
            return line.substr(0, i);
        }
    }
    return line;
}

}  // namespace

IniParse parseIni(std::string_view text)
{
    IniParse out;
    std::string section;
    bool in_section = false;
    std::size_t line_number = 0;

    while (!text.empty())
    {
        const std::size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        ++line_number;

        line = trim(stripComment(line));
        if (line.empty())
        {
            continue;
        }

        const std::string where = "line " + std::to_string(line_number);
        if (line.front() == '[')
        {
            if (line.back() != ']' || trim(line.substr(1, line.size() - 2)).empty())
            {
                out.problems.push_back(where + ": '" + std::string(line) + "' is not a section header");
                in_section = false;
                continue;
            }
            section = std::string(trim(line.substr(1, line.size() - 2)));
            out.sections[section];
            in_section = true;
            continue;
        }

        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos || trim(line.substr(0, equals)).empty())
        {
            out.problems.push_back(where + ": '" + std::string(line) + "' is not key=value");
            continue;
        }
        if (!in_section)
        {
            out.problems.push_back(where + ": '" + std::string(line) + "' is outside any section");
            continue;
        }

        out.sections[section][std::string(trim(line.substr(0, equals)))] =
            std::string(trim(line.substr(equals + 1)));
    }

    return out;
}

std::vector<std::string> splitList(std::string_view value)
{
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < value.size())
    {
        while (i < value.size() && isSpace(value[i]))
        {
            ++i;
        }
        const std::size_t start = i;
        while (i < value.size() && !isSpace(value[i]))
        {
            ++i;
        }
        if (i > start)
        {
            out.emplace_back(value.substr(start, i - start));
        }
    }
    return out;
}

std::expected<DisplayRecord, std::string> parseDisplayRecord(std::string_view text)
{
    IniParse ini = parseIni(text);

    const auto value = [&ini](const char* section, const char* key) -> std::string
    {
        const auto s = ini.sections.find(section);
        if (s == ini.sections.end())
        {
            return {};
        }
        const auto k = s->second.find(key);
        return k == s->second.end() ? std::string{} : k->second;
    };

    DisplayRecord record;
    record.role = value("display", "role");
    if (record.role.empty())
    {
        return std::unexpected(std::string("no [display] role, so this is not a display record"));
    }

    record.profile = value("display", "profile");
    record.name = value("display", "name");
    record.connector = value("display", "connector");
    record.nativeMode = value("display", "native_mode");

    record.backlightType = value("backlight", "type");
    record.backlightDevice = value("backlight", "device");
    if (const std::string max = value("backlight", "max"); !max.empty())
    {
        record.backlightMax = parseNumber(max);
        if (!record.backlightMax)
        {
            return std::unexpected("[backlight] max: '" + max + "' is not a number");
        }
    }

    record.ambientLightSensors = splitList(value("sensors", "ambient_light"));
    record.temperatureSensors = splitList(value("sensors", "temperature"));
    record.warnings = std::move(ini.problems);
    return record;
}

std::expected<DisplayRecord, RecordError> loadDisplayRecord(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        return std::unexpected(RecordError{RecordError::Kind::missing, path.string() + " does not exist"});
    }

    std::ifstream in(path);
    if (!in)
    {
        return std::unexpected(RecordError{RecordError::Kind::unreadable, "cannot open " + path.string()});
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();

    auto record = parseDisplayRecord(buffer.str());
    if (!record)
    {
        return std::unexpected(RecordError{RecordError::Kind::malformed, path.string() + ": " + record.error()});
    }
    return std::move(record.value());
}

}  // namespace display_backlight
