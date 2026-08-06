#ifndef BAG_PLAYBACK_H_
#define BAG_PLAYBACK_H_

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace bag
{

// Turning `--remap old=new` arguments into a lookup.
//
// A malformed entry is skipped and reported rather than aborting the replay:
// getting one of five remaps wrong should not throw away the other four. The
// caller learns which were bad through `problems`.
std::map<std::string, std::string> parseRemaps(const std::vector<std::string>& arguments,
                                               std::vector<std::string>& problems);

// The key a recorded message should be republished on.
//
// Applied in this order, and the order matters: a remap names a key AS RECORDED,
// so it has to be looked up before any prefix is bolted on. Doing it the other
// way round would make `--remap` silently stop matching the moment someone added
// `--prefix`.
//
// In libs/bag rather than in the `play` verb because getting it wrong is
// invisible: a prefix or remap that produces an unpublishable key means those
// messages are dropped, and a replay that is quietly missing a topic looks
// exactly like a recording that never had it. Testable logic belongs where it
// can be tested.
std::string resolvePlaybackKey(std::string_view recorded_key,
                               const std::map<std::string, std::string>& remaps,
                               std::string_view prefix);

}  // namespace bag

#endif  // BAG_PLAYBACK_H_
