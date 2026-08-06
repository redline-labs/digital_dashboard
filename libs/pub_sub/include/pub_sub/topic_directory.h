#ifndef PUB_SUB_TOPIC_DIRECTORY_H_
#define PUB_SUB_TOPIC_DIRECTORY_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pub_sub
{

// State common to every advertised thing, whatever kind it is.
//
// Factored out because the three directories below all answer the same two
// questions -- can I reach it, and has it been flapping -- and answering them
// differently in three places is how they drift apart.
struct DirectoryPresence
{
    // False once the advertiser went away. NOT removed from the directory --
    // see the class comment; a consumer greys the row rather than dropping it.
    bool reachable = true;

    // Monotonic counters, so a consumer can tell "this flickered" from "this
    // has been up the whole time" without keeping its own history.
    std::uint64_t appearances = 0;
    std::uint64_t disappearances = 0;
};

// One advertised topic, as the directory currently understands it.
struct DirectoryEntry : DirectoryPresence
{
    std::string key;
    std::string schema;

    // The zenoh session id of the publisher offering this topic. Join it
    // against NodeDirectory to get a name.
    //
    // EMPTY MEANS UNKNOWN, NOT UNOWNED. An advertisement from a build that
    // predates the zid carries no fifth segment, and the topic is no less
    // published for it. A consumer that renders empty as "no owner" is
    // reporting something it was not told.
    std::string owner_zid;
};

// One of our processes.
struct NodeEntry : DirectoryPresence
{
    std::string zid;
    std::string name;
};

// One callable service.
struct ServiceEntry : DirectoryPresence
{
    std::string key;
    std::string request_schema;
    std::string response_schema;
    std::string owner_zid;
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

// Which of our processes are alive, and what they are called.
//
// Watches '@redline/node/**', which pub_sub::NodeIdentity declares one token
// into per process. This is what turns the opaque zid on a topic advertisement
// -- and on every sample's timestamp -- into a name, and it is the only place a
// process that subscribes but never publishes (scope, the dashboard, the editor)
// shows up at all.
//
// Same rules as TopicDirectory: history is replayed, so a directory built after
// the nodes still sees them; entries are marked unreachable rather than removed,
// so "carplay was here and went away" stays visible; and snapshot() is a copy
// safe to call from any thread.
class NodeDirectory
{
  public:
    NodeDirectory();
    ~NodeDirectory();

    NodeDirectory(const NodeDirectory&) = delete;
    NodeDirectory& operator=(const NodeDirectory&) = delete;

    bool isValid() const;

    // Sorted by name, then zid -- so two instances of the same node sort
    // together, which is the case worth seeing.
    std::vector<NodeEntry> snapshot() const;

    std::uint64_t revision() const;

    // The name for a zid, or empty when this directory has not seen it.
    //
    // Empty is a real and common answer, not an error: zenoh reports peers that
    // are not ours at all, and any of our processes that has not declared a
    // NodeIdentity is equally anonymous. A caller should show the zid rather
    // than pretend the node does not exist.
    std::string nameFor(std::string_view zid) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Which queryable services can be called, and with what.
//
// Watches '@redline/svc/**', declared by pub_sub::ZenohService. Before this,
// services were undiscoverable -- zenoh would route a request to one, but
// nothing on the bus said it existed or what a request should contain.
class ServiceDirectory
{
  public:
    ServiceDirectory();
    ~ServiceDirectory();

    ServiceDirectory(const ServiceDirectory&) = delete;
    ServiceDirectory& operator=(const ServiceDirectory&) = delete;

    bool isValid() const;

    // Sorted by key.
    std::vector<ServiceEntry> snapshot() const;

    std::uint64_t revision() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pub_sub

#endif  // PUB_SUB_TOPIC_DIRECTORY_H_
