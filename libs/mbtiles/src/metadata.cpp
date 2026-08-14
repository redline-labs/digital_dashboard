// SPDX-License-Identifier: GPL-3.0-or-later
#include "mbtiles/metadata.h"

#include <cmath>
#include <cstdlib>

namespace mbtiles
{

std::optional<std::vector<double>> parseNumberList(const std::string& value, std::size_t expected)
{
    std::vector<double> out;
    out.reserve(expected);

    std::size_t start = 0;
    while (start <= value.size())
    {
        const std::size_t comma = value.find(',', start);
        const std::size_t end = (comma == std::string::npos) ? value.size() : comma;
        const std::string field = value.substr(start, end - start);

        // strtod rather than std::stod: stod throws, and a malformed metadata
        // row is an expected input here, not an exceptional one.
        const char* begin = field.c_str();
        char* stop = nullptr;
        const double parsed = std::strtod(begin, &stop);

        // Reject trailing junk ("32.3deg"), an empty field, and non-finite
        // values. Any of them would otherwise contribute a plausible number to
        // a bounding box that then renders somewhere else entirely.
        if (stop == begin || !std::isfinite(parsed))
        {
            return std::nullopt;
        }
        while (*stop == ' ' || *stop == '\t')
        {
            ++stop;
        }
        if (*stop != '\0')
        {
            return std::nullopt;
        }

        out.push_back(parsed);

        if (comma == std::string::npos)
        {
            break;
        }
        start = comma + 1;
    }

    if (out.size() != expected)
    {
        return std::nullopt;
    }

    return out;
}

} // namespace mbtiles
