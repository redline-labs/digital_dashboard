#ifndef BAG_METADATA_H_
#define BAG_METADATA_H_

#include "reflection/reflection.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bag
{

// What a recording is, beside its messages.
//
// A bag is a DIRECTORY, not a file: `metadata.yaml` plus one or more `.mcap`
// parts rolled at a size or duration limit. The split exists because a
// multi-gigabyte recording as a single file is awkward in every direction --
// copying it, resuming a transfer, and above all losing it: a writer killed
// mid-chunk damages the tail of whatever file it was writing, and with one file
// that is the whole recording.
//
// This index is what makes the split invisible to a reader. It also holds the
// things MCAP has no slot for -- how many messages were dropped, which topics
// were advertised but never published -- and those are exactly the facts a
// recording must not lose, because they are the difference between "there is no
// data for this topic" and "we failed to record it".
//
// A reflected struct, so the YAML both ways comes from config_codec like every
// other config in the tree rather than from a serializer written here.

// One rolled part.
REFLECT_STRUCT(bag_part_t,
    (std::string, path, "",
        "Path", "File name, relative to the bag directory"),
    (std::uint64_t, bytes, 0,
        "Bytes", "Size on disk"),
    (std::uint64_t, message_count, 0,
        "Messages", "Messages in this part"),
    (std::uint64_t, t_begin_ns, 0,
        "Begin", "Log time of the first message, ns since the UNIX epoch"),
    (std::uint64_t, t_end_ns, 0,
        "End", "Log time of the last message, ns since the UNIX epoch"),
    (bool, complete, true,
        "Complete", "False when the part has no summary -- i.e. its writer died")
)

// One topic that appeared in the recording.
REFLECT_STRUCT(bag_topic_t,
    (std::string, key, "",
        "Key", "The zenoh key"),
    (std::string, schema, "",
        "Schema", "Registry schema name"),
    (std::uint64_t, message_count, 0,
        "Messages", "How many were recorded"),
    (std::string, origin_zid, "",
        "Origin", "Session that published them; '(mixed)' if more than one did"),
    (bool, advertised_only, false,
        "Silent", "Advertised for the whole recording and never published")
)

REFLECT_STRUCT(bag_metadata_t,
    (std::uint32_t, version, 1,
        "Version", "Layout version of this file"),
    (std::string, created, "",
        "Created", "ISO-8601 local time the recording started"),
    (std::string, recorder, "",
        "Recorder", "Tool and version that wrote it"),
    (std::string, compression, "zstd",
        "Compression", "Chunk codec: none, lz4 or zstd"),
    (std::uint64_t, message_count, 0,
        "Messages", "Total across all parts"),
    (std::uint64_t, t_begin_ns, 0,
        "Begin", "Log time of the earliest message"),
    (std::uint64_t, t_end_ns, 0,
        "End", "Log time of the latest message"),

    // NOT cosmetic. A recorder that could not keep up drops messages, and a
    // recording that lost samples silently is worse than one that says it did --
    // a gap in a trace reads as "the publisher stopped", which is a completely
    // different fault to chase.
    (std::uint64_t, dropped_messages, 0,
        "Dropped", "Messages the recorder could not keep up with"),

    // How many messages had no publish timestamp and had to borrow their
    // arrival time. A bag where this is large has a publish_time that is really
    // a log_time, and anything computing latency from it would be measuring
    // nothing.
    (std::uint64_t, unstamped_messages, 0,
        "Unstamped", "Messages whose publish_time was synthesised from log_time"),

    (std::vector<bag_part_t>, parts, {},
        "Parts", "The .mcap files, in time order"),
    (std::vector<bag_topic_t>, topics, {},
        "Topics", "Everything seen or advertised during the recording")
)

// metadata.yaml inside `directory`.
std::string metadataPath(const std::string& directory);

// Returns nullopt when the file is missing or does not parse. A bag whose index
// is unreadable is not usable as a bag -- but see `bag reindex`, which rebuilds
// one from the parts.
//
// `quiet` suppresses the "no metadata.yaml" error. That is for `reindex`, whose
// whole job is to run against a directory that has none -- reporting the absence
// as an error there tells the user their command failed when it is doing exactly
// what they asked.
std::optional<bag_metadata_t> loadMetadata(const std::string& directory, bool quiet = false);

// Written to a temporary and renamed, so a crash mid-write leaves the previous
// index intact rather than a half-written one. The recorder rewrites this after
// every roll, so "mid-write" is not hypothetical.
bool saveMetadata(const bag_metadata_t& metadata, const std::string& directory);

}  // namespace bag

#endif  // BAG_METADATA_H_
