#include "inspect/verbs.h"

#include "cli/output.h"

#include "pub_sub/session_manager.h"
#include "pub_sub/topic_directory.h"

#include <zenoh.hxx>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <thread>

namespace inspect
{

void addNodesOptions(cxxopts::Options& options)
{
    options.add_options()
        ("all", "Include nodes that have gone away.",
            cxxopts::value<bool>()->default_value("false")->implicit_value("true"));
}

int runNodes(cli::Context& context)
{
    const bool include_gone = context.flag("all");

    auto session = pub_sub::SessionManager::getOrCreate();
    if (!session)
    {
        SPDLOG_ERROR("No zenoh session available.");
        return cli::kFailure;
    }

    // Two different questions, joined on the zid.
    //
    // Zenoh answers "which sessions can I see" and returns opaque ids -- which
    // was all this verb could ever print. The node directory answers "what are
    // OUR processes called", from the liveliness token each declares. Neither
    // alone is useful: zenoh's list has no names, and ours has no knowledge of
    // sessions that are not ours (a router, another tool, a stray zenohd).
    pub_sub::NodeDirectory nodes;
    pub_sub::TopicDirectory topics;

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    std::set<std::string> peer_ids;
    std::set<std::string> router_ids;
    try
    {
        for (const auto& id : session->get_peers_z_id())
        {
            peer_ids.insert(id.to_string());
        }
        for (const auto& id : session->get_routers_z_id())
        {
            router_ids.insert(id.to_string());
        }
    }
    catch (const std::exception& e)
    {
        SPDLOG_WARN("Could not enumerate zenoh sessions: {}", e.what());
    }

    const std::string own_zid = session->get_zid().to_string();

    // How many topics each session publishes, so the listing says what a node
    // actually does rather than only that it exists.
    std::map<std::string, std::size_t> topics_by_owner;
    for (const pub_sub::DirectoryEntry& entry : topics.snapshot())
    {
        if (entry.reachable && !entry.owner_zid.empty())
        {
            ++topics_by_owner[entry.owner_zid];
        }
    }

    struct Row
    {
        std::string zid;
        std::string name;
        std::string role;
        bool reachable = true;
        std::size_t topics = 0;
        bool is_self = false;
    };

    std::vector<Row> rows;
    std::set<std::string> covered;

    for (const pub_sub::NodeEntry& entry : nodes.snapshot())
    {
        if (!entry.reachable && !include_gone)
        {
            continue;
        }
        Row row;
        row.zid = entry.zid;
        row.name = entry.name;
        row.reachable = entry.reachable;
        row.topics = topics_by_owner.count(entry.zid) ? topics_by_owner[entry.zid] : 0;
        row.is_self = entry.zid == own_zid;
        row.role = router_ids.count(entry.zid) ? "router" : "peer";
        rows.push_back(std::move(row));
        covered.insert(entry.zid);
    }

    // This process. get_peers_z_id() reports the sessions we are connected TO,
    // never our own, and inspect declares no NodeIdentity (a transient tool
    // should not clutter the node list) -- so without this the listing silently
    // omits the one session the reader definitely knows about, and the '*'
    // legend below is never true of any row.
    if (!covered.count(own_zid))
    {
        Row row;
        row.zid = own_zid;
        row.name = "inspect";
        row.role = "peer";
        row.is_self = true;
        rows.push_back(std::move(row));
        covered.insert(own_zid);
    }

    // Sessions zenoh knows about that declared no NodeIdentity. Listed rather
    // than hidden: a zenohd router, another tool, or one of our own nodes that
    // has not been given an identity yet all show up here, and "there is
    // something on the bus I cannot name" is worth knowing.
    for (const std::set<std::string>* group : {&peer_ids, &router_ids})
    {
        for (const std::string& id : *group)
        {
            if (covered.count(id))
            {
                continue;
            }
            Row row;
            row.zid = id;
            row.role = (group == &router_ids) ? "router" : "peer";
            row.topics = topics_by_owner.count(id) ? topics_by_owner[id] : 0;
            rows.push_back(std::move(row));
            covered.insert(id);
        }
    }

    std::sort(rows.begin(), rows.end(),
              [](const Row& lhs, const Row& rhs)
              {
                  if (lhs.name.empty() != rhs.name.empty())
                  {
                      return !lhs.name.empty();  // named first
                  }
                  if (lhs.name != rhs.name)
                  {
                      return lhs.name < rhs.name;
                  }
                  return lhs.zid < rhs.zid;
              });

    if (context.json())
    {
        nlohmann::json out = nlohmann::json::array();
        for (const Row& row : rows)
        {
            nlohmann::json entry;
            entry["zid"] = row.zid;
            entry["name"] = row.name;
            entry["role"] = row.role;
            entry["reachable"] = row.reachable;
            entry["topics_published"] = row.topics;
            entry["self"] = row.is_self;
            out.push_back(std::move(entry));
        }
        cli::out("{}", out.dump(2));
        return cli::kOk;
    }

    if (rows.empty())
    {
        cli::out("Nothing else is on the bus.");
        return cli::kOk;
    }

    cli::out("{:<24} {:<34} {:<8} {}", "NODE", "SESSION", "ROLE", "TOPICS");
    for (const Row& row : rows)
    {
        std::string name = row.name.empty() ? "(unnamed)" : row.name;
        if (row.is_self)
        {
            name += " *";
        }
        if (!row.reachable)
        {
            name += " [gone]";
        }
        cli::out("{:<24} {:<34} {:<8} {}", name, row.zid, row.role,
                 row.topics == 0 ? std::string("-") : std::to_string(row.topics));
    }

    cli::out("");
    cli::out("* this process. An unnamed session declared no pub_sub::NodeIdentity -- it may be");
    cli::out("  a router, another tool, or one of ours that does not announce itself.");

    return cli::kOk;
}

}  // namespace inspect
