// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef PLIST_XML_H_
#define PLIST_XML_H_

#include "plist/value.h"

#include <optional>
#include <string>

namespace plist
{

// Serializes to an Apple XML property list, DOCTYPE and all. Tab-indented, in
// libplist's layout, which the peers reading it are written against -- for
// scalar and container elements that match is byte for byte.
//
// `<data>` is the exception: libplist wraps base64 at 68 columns and this wraps
// at 60. Both parse either way, so the difference is cosmetic; it is called out
// because the test suite asserts the match for everything else.
std::string encodeXml(const Value& root);

// Parses an Apple XML property list. Returns nullopt when the document is
// malformed, unterminated, or contains an element outside the plist vocabulary.
//
// Deliberately tolerant of the things real writers vary on: the XML declaration,
// the DOCTYPE (with or without an internal subset), comments, any whitespace or
// indentation, and `<data>` split across lines. Deliberately intolerant of
// anything else -- a peer sending an element we do not model is a protocol
// mismatch worth failing on rather than silently dropping.
std::optional<Value> decodeXml(std::string_view xml);

}  // namespace plist

#endif  // PLIST_XML_H_
