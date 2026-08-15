// SPDX-License-Identifier: GPL-3.0-or-later
//
// What goes wrong decoding a Mapbox Vector Tile.
//
// The type moved to libs/protowire when libs/osm needed the same reader, and
// this header re-exports it so nothing that says `mvt::Error` had to change.
// The alias is not a deprecation: inside this library the errors ARE vector
// tile errors, and reading them as such is the point.
#ifndef MVT_ERROR_H
#define MVT_ERROR_H

#include "protowire/error.h"

namespace mvt
{

using Error = protowire::Error;

template <typename T>
using Result = protowire::Result<T>;

using protowire::malformed;
using protowire::truncated;
using protowire::unsupported;
using protowire::decompress_failed;
using protowire::to_string;

} // namespace mvt

#endif // MVT_ERROR_H
