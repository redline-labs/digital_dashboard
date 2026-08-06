#ifndef PUB_SUB_NODE_IDENTITY_H_
#define PUB_SUB_NODE_IDENTITY_H_

#include <memory>
#include <string>
#include <string_view>

namespace pub_sub
{

// Announces this process on the bus for as long as it lives.
//
//     int main(int argc, char** argv)
//     {
//         pub_sub::NodeIdentity node("carplay");
//         ...
//     }
//
// WHY THIS EXISTS. A topic advertisement now carries the zid of the publisher
// that offers it, which makes "which process owns this topic" answerable -- but
// only as far as an opaque hex id. This is the other half: it maps that id to a
// name a person recognises. Without it, `inspect nodes` can list what zenoh
// knows (get_peers_z_id(), get_routers_z_id()) and print nothing but ids, which
// is what it did.
//
// It is also the ONLY way a process that subscribes but never publishes appears
// on the bus at all. Scope, the dashboard and the editor declare no publishers,
// so they had no advertisement of any kind and were invisible to every tool --
// including to each other.
//
// DECLARED EXPLICITLY, not derived. SessionManager could have announced every
// process automatically using the executable name, and that was tempting because
// then nothing can forget. It is not done because the executable name is not the
// node name: several nodes are launched from the same binary with different
// arguments, the test binaries would announce themselves as nodes, and deriving
// it needs a different syscall per platform. A one-line declaration in main()
// says what the process *is*, and a process without one is simply not
// advertised -- which is honest, rather than advertised under a name that came
// from a path.
//
// The name goes through isValidTopicKey(), so a name with a '/' or a '%' in it
// is refused loudly at startup rather than producing an unparseable key that
// every reader silently skips.
class NodeIdentity
{
  public:
    explicit NodeIdentity(std::string_view node_name);
    ~NodeIdentity();

    NodeIdentity(const NodeIdentity&) = delete;
    NodeIdentity& operator=(const NodeIdentity&) = delete;
    NodeIdentity(NodeIdentity&&) = delete;
    NodeIdentity& operator=(NodeIdentity&&) = delete;

    // False when the token could not be declared -- no session, or a name that
    // is not a usable key segment. The process still works; it is just not
    // listed. Worth checking in anything whose job is to be discoverable.
    bool isValid() const;

    // This process's session id, the join key against topic advertisements.
    // Empty when construction failed.
    std::string_view zid() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pub_sub

#endif  // PUB_SUB_NODE_IDENTITY_H_
