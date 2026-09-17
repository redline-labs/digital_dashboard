// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sends a page_command_t to the page_stack it names. Shared by page_button and
// the CarPlay widget's return button, so "change the page" is one code path
// whichever control asked for it.
#ifndef DASHBOARD_PAGE_COMMAND_PUBLISHER_H_
#define DASHBOARD_PAGE_COMMAND_PUBLISHER_H_

#include "dashboard/page_command.h"

#include "dashboard_pages.capnp.h"
#include "pub_sub/topic_key.h"
#include "pub_sub/zenoh_publisher.h"

#include <spdlog/spdlog.h>

#include <map>
#include <memory>
#include <string>

namespace dashboard
{

inline PageStackCommand::Action toCapnp(page_action_t action)
{
    switch (action)
    {
        case page_action_t::next:
            return PageStackCommand::Action::NEXT;
        case page_action_t::prev:
            return PageStackCommand::Action::PREV;
        case page_action_t::go_to:
            return PageStackCommand::Action::GO_TO;
        case page_action_t::back:
            return PageStackCommand::Action::BACK;
    }
    return PageStackCommand::Action::NEXT;
}

inline page_action_t fromCapnp(PageStackCommand::Action action)
{
    switch (action)
    {
        case PageStackCommand::Action::NEXT:
            return page_action_t::next;
        case PageStackCommand::Action::PREV:
            return page_action_t::prev;
        case PageStackCommand::Action::GO_TO:
            return page_action_t::go_to;
        case PageStackCommand::Action::BACK:
            return page_action_t::back;
    }
    return page_action_t::next;
}

// One publisher per target, declared on first use. GUI thread only, like every
// ZenohPublisher: the builder inside is not shared-safe.
class PageCommandSender
{
  public:
    // False when the command could not be sent: no target, or a target that is
    // not usable in a key. Logged, because a button that does nothing is
    // otherwise indistinguishable from one nobody is listening to.
    bool send(const page_command_t& command)
    {
        const std::string key = pageCommandKey(command.target);
        if (command.target.empty() || !pub_sub::isValidTopicKey(key))
        {
            SPDLOG_WARN("Page command has no usable target ('{}'); not sent.", command.target);
            return false;
        }

        auto it = publishers_.find(key);
        if (it == publishers_.end())
        {
            it = publishers_.emplace(key, std::make_unique<pub_sub::ZenohPublisher<PageStackCommand>>(key)).first;
        }

        auto& fields = it->second->fields();
        fields.setAction(toCapnp(command.action));
        fields.setPage(command.page);
        it->second->put();
        SPDLOG_INFO("Sent page command {} '{}' to '{}'.", reflection::enum_to_string(command.action),
                    command.page, command.target);
        return true;
    }

  private:
    std::map<std::string, std::unique_ptr<pub_sub::ZenohPublisher<PageStackCommand>>> publishers_;
};

}  // namespace dashboard

#endif  // DASHBOARD_PAGE_COMMAND_PUBLISHER_H_
