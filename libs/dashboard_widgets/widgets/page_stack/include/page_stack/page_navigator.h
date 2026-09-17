// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which page a page_stack shows, and what each command does to that. No Qt, no
// bus: the widget owns one and asks it, so every rule is testable on its own.
#ifndef PAGE_STACK_PAGE_NAVIGATOR_H_
#define PAGE_STACK_PAGE_NAVIGATOR_H_

#include "dashboard/page_command.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace page_stack
{

class PageNavigator
{
  public:
    struct Page
    {
        std::string name;
        // Whether next/prev stop here. go_to and back reach every page.
        bool in_cycle = true;
    };

    enum class Result
    {
        changed,
        // A valid command that leaves the page where it is: go_to the page
        // already shown, back with nothing to go back to, a cycle with one stop.
        unchanged,
        unknown_page,
        missing_page_name,
        // No pages at all; nothing can be shown.
        empty,
    };

    // An empty or unknown `default_page` starts on the first page. Validation
    // refuses an unknown one before it gets here; this is only the fallback.
    PageNavigator(std::vector<Page> pages, std::string_view default_page)
        : pages_(std::move(pages))
    {
        if (const auto index = find(default_page))
        {
            current_ = *index;
        }
    }

    Result apply(page_action_t action, std::string_view page)
    {
        if (pages_.empty())
        {
            return Result::empty;
        }

        switch (action)
        {
            case page_action_t::next:
                return moveTo(stepInCycle(+1));
            case page_action_t::prev:
                return moveTo(stepInCycle(-1));
            case page_action_t::go_to:
            {
                if (page.empty())
                {
                    return Result::missing_page_name;
                }
                const auto index = find(page);
                if (!index)
                {
                    return Result::unknown_page;
                }
                return moveTo(*index);
            }
            case page_action_t::back:
                return previous_ ? moveTo(*previous_) : Result::unchanged;
        }
        return Result::unchanged;
    }

    bool empty() const { return pages_.empty(); }
    std::size_t current() const { return current_; }
    std::optional<std::size_t> previous() const { return previous_; }
    const std::vector<Page>& pages() const { return pages_; }

    std::optional<std::size_t> find(std::string_view name) const
    {
        for (std::size_t i = 0; i < pages_.size(); ++i)
        {
            if (pages_[i].name == name)
            {
                return i;
            }
        }
        return std::nullopt;
    }

  private:
    // The nearest in-cycle page in `direction`, wrapping; the current page when
    // nothing else qualifies. Walks from the current page's POSITION, so a page
    // outside the cycle still moves on to its neighbour rather than to the start.
    std::size_t stepInCycle(int direction) const
    {
        const std::size_t count = pages_.size();
        for (std::size_t step = 1; step < count; ++step)
        {
            const std::size_t index = direction > 0 ? (current_ + step) % count
                                                    : (current_ + count - step) % count;
            if (pages_[index].in_cycle)
            {
                return index;
            }
        }
        return current_;
    }

    // `previous` only moves on a real change: go_to the page already shown
    // must not erase where `back` would have gone.
    Result moveTo(std::size_t index)
    {
        if (index == current_)
        {
            return Result::unchanged;
        }
        previous_ = current_;
        current_ = index;
        return Result::changed;
    }

    std::vector<Page> pages_;
    std::size_t current_ = 0;
    std::optional<std::size_t> previous_;
};

}  // namespace page_stack

#endif  // PAGE_STACK_PAGE_NAVIGATOR_H_
