#include "pub_sub/node_identity.h"

#include "pub_sub/session_manager.h"
#include "pub_sub/topic_key.h"

#include <zenoh.hxx>

#include <spdlog/spdlog.h>

#include <optional>

namespace pub_sub
{

struct NodeIdentity::Impl
{
    std::string zid;

    // Held so the session outlives the token declared on it.
    std::shared_ptr<zenoh::Session> session;

    // Declared last so it is undeclared first, before the session it belongs to
    // goes away.
    std::optional<zenoh::LivelinessToken> token;
};

NodeIdentity::NodeIdentity(std::string_view node_name) : impl_(std::make_unique<Impl>())
{
    // The same charset rule as a topic key, and checked for the same reason: a
    // name containing '/' would split into extra segments and be read back as a
    // different name, and one containing '@' or a wildcard character would
    // produce a key no reader can match. Every one of those fails by the node
    // simply not appearing, which is indistinguishable from it not running.
    if (const std::string problem = topicKeyProblem(node_name); !problem.empty())
    {
        SPDLOG_CRITICAL("Refusing to announce this process as '{}': {}. The node will run but "
                        "will not be listed on the bus.",
                        node_name, problem);
        return;
    }

    // A name with a '/' in it is a valid *topic key* but not a valid single
    // segment, and the node space has no mangling -- a node name is not a
    // hierarchy. Rejected separately so the message can say which rule was
    // broken.
    if (node_name.find('/') != std::string_view::npos)
    {
        SPDLOG_CRITICAL("Refusing to announce this process as '{}': a node name is a single "
                        "segment and may not contain '/'.",
                        node_name);
        return;
    }

    impl_->session = SessionManager::getOrCreate();
    if (!impl_->session)
    {
        SPDLOG_ERROR("No zenoh session available to announce node '{}'", node_name);
        return;
    }

    try
    {
        impl_->zid = impl_->session->get_zid().to_string();

        const std::string key = nodeKey(impl_->zid, node_name);
        impl_->token.emplace(impl_->session->liveliness_declare_token(zenoh::KeyExpr(key)));
        SPDLOG_DEBUG("Announced node '{}' as '{}'", node_name, key);
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Failed to announce node '{}': {}", node_name, e.what());
        impl_->token.reset();
    }
}

NodeIdentity::~NodeIdentity() = default;

bool NodeIdentity::isValid() const
{
    return impl_->token.has_value();
}

std::string_view NodeIdentity::zid() const
{
    return impl_->zid;
}

}  // namespace pub_sub
