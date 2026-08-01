// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/stack/bplist.ts
#ifndef PLIST_BINARY_H_
#define PLIST_BINARY_H_

#include "plist/value.h"

#include <optional>

namespace plist
{

// Serializes to a complete bplist00 document. Returns an empty vector on
// failure (only possible for pathologically large inputs).
Bytes encodeBinary(const Value& root);

// Parses a bplist00 document. Returns nullopt on a malformed or truncated
// buffer, or on an object type outside the supported subset.
std::optional<Value> decodeBinary(const Bytes& buffer);

// True when the buffer opens with the "bplist00" magic. Lockdown and usbmux
// peers are free to answer in either format, so readers sniff rather than
// assume.
bool looksBinary(const Bytes& buffer);

}  // namespace plist

#endif  // PLIST_BINARY_H_
