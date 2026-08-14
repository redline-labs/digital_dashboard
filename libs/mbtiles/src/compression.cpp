// SPDX-License-Identifier: GPL-3.0-or-later
#include "mbtiles/compression.h"

namespace mbtiles
{

const char* to_string(Encoding encoding)
{
    switch (encoding)
    {
        case Encoding::Identity:
            return "identity";
        case Encoding::Gzip:
            return "gzip";
        case Encoding::Deflate:
            return "deflate";
        case Encoding::Zstd:
            return "zstd";
    }

    return "unknown";
}

} // namespace mbtiles
