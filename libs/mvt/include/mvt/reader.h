// SPDX-License-Identifier: GPL-3.0-or-later
//
// Just enough protobuf to read a vector tile.
//
// The reader moved to libs/protowire when libs/osm needed it too -- an OSM
// extractor depending on a vector-tile library would have been a nonsense
// dependency edge. This header re-exports it so nothing that says `mvt::Reader`
// had to change. See protowire/reader.h for what it does and does not do.
#ifndef MVT_READER_H
#define MVT_READER_H

#include "mvt/error.h"
#include "protowire/reader.h"

namespace mvt
{

using protowire::Field;
using protowire::Reader;
using protowire::WireType;
using protowire::unzigzag;

} // namespace mvt

#endif // MVT_READER_H
