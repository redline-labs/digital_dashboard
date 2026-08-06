#ifndef PUB_SUB_TOPIC_DIRECTORY_H_
#define PUB_SUB_TOPIC_DIRECTORY_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pub_sub
{

// One advertised topic, as the directory currently understands it.
struct DirectoryEntry
{
    std::string key;
    std::string schema;

    // False once the advertiser went away. NOT removed from the directory --
    // see the class comment; a consumer greys the row rather than dropping it.
    bool reachable = true;

    // Monotonic counters, so a consumer can tell "this flickered" from "this
    // has been up the whole time" without keeping its own history.
    std::uint64_t appearances = 0;
    std::uint64_t disappearances = 0;
};

// What is advertised on the bus, kept current with no polling.
//
// Every publisher declares a zenoh liveliness token when it is constructed (see
// detail::BytePublisher). This watches that key space and maintains the set. A
// consumer gets a topic list that is populated the moment a node starts --
// including topics that have never published a sample, which is the thing a
// traffic-observing discovery can never do, because a topic that has said
// nothing is indistinguishable from one that does not exist.
//
// WHAT THIS IS NOT: a statement that data is flowing. A liveliness token is up
// while its process is, so a CAN bridge with an unplugged adapter still
// advertises its topics. That is the right answer for a picker -- the topic
// genuinely exists and is bindable -- but a consumer that wants "is this
// producing" has to observe traffic as well. The two are complementary and the
// scope browser shows both.
//
// ENTRIES ARE NEVER REMOVED, only marked unreachable. A picker that evicted a
// row would take away a signal the user may already have bound, or may be about
// to; and since a DELETE means "unreachable from here" rather than "gone"
// (a network partition looks identical to a crash), removing it would also
// claim more than zenoh actually told us.
class TopicDirectory
{
  public:
    // Starts watching immediately. Existing advertisements are delivered too --
    // the subscription is declared with history, so a directory created after
    // the publishers still sees them, and there is no initial query to forget.
    TopicDirectory();
    ~TopicDirectory();

    TopicDirectory(const TopicDirectory&) = delete;
    TopicDirectory& operator=(const TopicDirectory&) = delete;

    // False when the subscription could not be declared, in which case the
    // directory stays empty rather than looking merely idle.
    bool isValid() const;

    // A copy, sorted by key. Callable from any thread; the updates arrive on a
    // zenoh thread, so a consumer polls this from wherever it can safely draw.
    //
    // Returning a copy rather than a reference is deliberate: a consumer that
    // held a reference would be reading a container being mutated underneath it.
    std::vector<DirectoryEntry> snapshot() const;

    // How many times the directory has changed. A consumer can skip rebuilding
    // its view when this has not moved, which is what makes polling it on a GUI
    // timer cheap.
    std::uint64_t revision() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pub_sub

#endif  // PUB_SUB_TOPIC_DIRECTORY_H_
