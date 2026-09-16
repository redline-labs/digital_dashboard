// SPDX-License-Identifier: GPL-3.0-or-later
//
// A fingerprint of what a schema's bytes MEAN, so a publisher and a subscriber
// that disagree can find out.
//
// The schema name on every sample says which message this is. It does not say
// which VERSION of that message: widen a field from Int8 to Int16, reorder a
// union, change a list's element type, and the name is unchanged while every
// field after it lands on different bytes. capnp does not throw -- it reads
// whatever is there -- so the symptom is a plausible wrong number, which is the
// one failure this tree keeps saying it will not accept.
//
// The fingerprint is computed from the compiled schema, not from the .capnp
// text: comments, field order in the file and formatting do not change it, and
// anything that moves a byte does.
//
// For a schema this build registers, it is computed when the schemas are
// compiled and reaches the program as a constant. Only a descriptor from
// SOMEWHERE ELSE -- a recording written by another build -- is hashed at run
// time, because that is the only one whose value this build cannot know.
#ifndef PUB_SUB_SCHEMA_LAYOUT_H_
#define PUB_SUB_SCHEMA_LAYOUT_H_

#include <capnp/schema.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace pub_sub
{

// Zero is never a valid fingerprint, so it doubles as "unknown".
inline constexpr std::uint64_t kNoLayout = 0;

// Over the struct's own id, and for each field its name, index, ordinal,
// discriminant and type -- following struct and list-of-struct fields to a
// bounded depth, so a change inside a nested message counts too.
//
// Defined in schema_layout_hash.cpp, which is compiled into both this library
// and the registry generator: the constants below are produced by this exact
// function at build time.
std::uint64_t layoutHash(capnp::StructSchema schema);

// The fingerprint of a registered schema. nullopt when the name is not in this
// build's registry, which is how a sample from a newer node reads.
//
// A lookup, not a computation. The generator hashed every registered schema
// while it had capnp's parse in hand, so the answer is a constant in the
// generated registry.
//
// Prefer pub_sub::schema_traits<T>::layout wherever the schema is known as a
// type, which is nearly everywhere: both the typed publisher and the typed
// subscriber use it. This is for the callers that genuinely have only a name --
// `bag play`, republishing whatever a recording holds.
std::optional<std::uint64_t> layoutHashFor(std::string_view schema_name);

// The fingerprint of a schema as some OTHER build defined it, from a stored
// descriptor -- the serialized CodeGeneratorRequest a recording carries beside
// the bytes it recorded. nullopt when the descriptor is empty, unreadable, or
// does not contain the named schema.
//
// This is how a replay notices that a field has moved since the recording was
// made: same name, different bytes.
std::optional<std::uint64_t> layoutHashOfDescriptor(std::span<const std::uint8_t> descriptor,
                                                    std::string_view schema_name);

}  // namespace pub_sub

#endif  // PUB_SUB_SCHEMA_LAYOUT_H_
