#ifndef BAG_REBUILD_H_
#define BAG_REBUILD_H_

#include "bag/metadata.h"

#include <optional>
#include <string>

namespace bag
{

// Reconstructs a bag's index from the .mcap files on disk.
//
// metadata.yaml is written when the recorder closes and after every roll, so a
// recorder that was killed leaves parts that no index describes -- and BagReader
// finds parts through metadata.yaml and nowhere else, so those parts are
// unreadable until this runs. A recording that ended in a crash is the one most
// likely to matter, which makes this a recovery path rather than a convenience.
//
// Lives in the library rather than in `bag reindex` so it can be tested without
// driving a binary. The verb is a thin wrapper over it.
//
// Preserves from any existing index the things the files cannot say: the drop
// count above all. A rebuild that reset that to zero would silently claim a
// lossy recording was complete.
//
// What it CANNOT restore, and does not pretend to: topics that were advertised
// and never published. Only the live recorder knew about those; nothing in a
// file records a topic that produced no message.
//
// Returns nullopt when the directory holds no readable .mcap files at all.
std::optional<bag_metadata_t> rebuildMetadata(const std::string& directory);

}  // namespace bag

#endif  // BAG_REBUILD_H_
