#include "editor/page_edits.h"

#include "dashboard/page_command_walk.h"
#include "dashboard/widget_registry.h"

#include <algorithm>
#include <utility>
#include <variant>
#include <vector>

namespace editor::page_edits
{

namespace
{

widget_config_t* stackIn(Snapshot& state, std::size_t index)
{
    if (state.active_window >= state.doc.windows.size())
    {
        return nullptr;
    }
    auto& widgets = state.doc.windows[state.active_window].widgets;
    if (index >= widgets.size() || widgets[index].type() != widget_type_t::page_stack)
    {
        return nullptr;
    }
    return &widgets[index];
}

// The names of the widgets on each of the stack's pages, sized to its pages so
// an edit never indexes past a snapshot taken without them.
std::vector<std::vector<QString>>& pageNamesOf(Snapshot& state, std::size_t index, std::size_t pages)
{
    if (state.page_names.size() <= index)
    {
        state.page_names.resize(index + 1);
    }
    auto& names = state.page_names[index];
    if (names.size() < pages)
    {
        names.resize(pages);
    }
    return names;
}

bool nameTaken(const widget_config_t& stack, const std::string& name)
{
    return std::any_of(stack.pages.begin(), stack.pages.end(),
                       [&](const widget_page_t& page) { return page.name == name; });
}

void renameCommandsIn(widget_config_t& widget, const std::string& stack_id, const std::string& old_name,
                      const std::string& name)
{
    dashboard::forEachPageCommand(widget, [&](page_command_t& command) {
        if (command.target == stack_id && command.page == old_name)
        {
            command.page = name;
        }
    });
}

template <typename List>
void reorder(List& list, std::size_t from, std::size_t to)
{
    auto item = std::move(list[from]);
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(from));
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(to), std::move(item));
}

}  // namespace

std::optional<std::size_t> addPage(Snapshot& state, std::size_t stack, std::string name)
{
    widget_config_t* cfg = stackIn(state, stack);
    if (cfg == nullptr)
    {
        return std::nullopt;
    }
    if (name.empty())
    {
        for (std::size_t n = cfg->pages.size() + 1; name.empty() || nameTaken(*cfg, name); ++n)
        {
            name = "page_" + std::to_string(n);
        }
    }
    else if (nameTaken(*cfg, name))
    {
        return std::nullopt;
    }
    pageNamesOf(state, stack, cfg->pages.size());
    widget_page_t page;
    page.name = std::move(name);
    cfg->pages.push_back(std::move(page));
    state.page_names[stack].emplace_back();
    return cfg->pages.size() - 1;
}

bool removePage(Snapshot& state, std::size_t stack, std::size_t page)
{
    widget_config_t* cfg = stackIn(state, stack);
    if (cfg == nullptr || page >= cfg->pages.size() || cfg->pages.size() <= 1)
    {
        return false;
    }
    // A default that no longer exists would not load; the first page it is.
    if (auto* stack_cfg = std::get_if<PageStackWidget::config_t>(&cfg->config);
        stack_cfg != nullptr && stack_cfg->default_page == cfg->pages[page].name)
    {
        stack_cfg->default_page.clear();
    }
    auto& names = pageNamesOf(state, stack, cfg->pages.size());
    cfg->pages.erase(cfg->pages.begin() + static_cast<std::ptrdiff_t>(page));
    names.erase(names.begin() + static_cast<std::ptrdiff_t>(page));
    return true;
}

bool renamePage(Snapshot& state, std::size_t stack, std::size_t page, const std::string& name)
{
    widget_config_t* cfg = stackIn(state, stack);
    if (cfg == nullptr || name.empty() || page >= cfg->pages.size() || nameTaken(*cfg, name))
    {
        return false;
    }
    const std::string old_name = cfg->pages[page].name;
    const std::string stack_id = cfg->id;
    cfg->pages[page].name = name;

    if (auto* stack_cfg = std::get_if<PageStackWidget::config_t>(&cfg->config))
    {
        if (stack_cfg->default_page == old_name)
        {
            stack_cfg->default_page = name;
        }
        for (page_trigger_t& trigger : stack_cfg->triggers)
        {
            if (trigger.page == old_name)
            {
                trigger.page = name;
            }
        }
    }
    // A button can only aim at a stack with an id; one without has none to
    // follow.
    if (!stack_id.empty())
    {
        for (app_config_t& window : state.doc.windows)
        {
            for (widget_config_t& widget : window.widgets)
            {
                renameCommandsIn(widget, stack_id, old_name, name);
                for (widget_page_t& other : widget.pages)
                {
                    for (widget_config_t& child : other.widgets)
                    {
                        renameCommandsIn(child, stack_id, old_name, name);
                    }
                }
            }
        }
    }
    return true;
}

bool setPageInCycle(Snapshot& state, std::size_t stack, std::size_t page, bool in_cycle)
{
    widget_config_t* cfg = stackIn(state, stack);
    if (cfg == nullptr || page >= cfg->pages.size() || cfg->pages[page].in_cycle == in_cycle)
    {
        return false;
    }
    cfg->pages[page].in_cycle = in_cycle;
    return true;
}

bool movePage(Snapshot& state, std::size_t stack, std::size_t from, std::size_t to)
{
    widget_config_t* cfg = stackIn(state, stack);
    if (cfg == nullptr || from >= cfg->pages.size() || to >= cfg->pages.size() || from == to)
    {
        return false;
    }
    auto& names = pageNamesOf(state, stack, cfg->pages.size());
    reorder(cfg->pages, from, to);
    reorder(names, from, to);
    return true;
}

bool moveWidgetToPage(Snapshot& state, std::size_t stack, std::size_t from, std::size_t index,
                      std::size_t to)
{
    widget_config_t* cfg = stackIn(state, stack);
    if (cfg == nullptr || from == to || from >= cfg->pages.size() || to >= cfg->pages.size() ||
        index >= cfg->pages[from].widgets.size())
    {
        return false;
    }
    auto& names = pageNamesOf(state, stack, cfg->pages.size());
    // The name travels with the widget, so the canvas moves it rather than
    // destroying one and building another.
    if (index >= names[from].size())
    {
        return false;
    }
    auto& from_widgets = cfg->pages[from].widgets;
    cfg->pages[to].widgets.push_back(std::move(from_widgets[index]));
    from_widgets.erase(from_widgets.begin() + static_cast<std::ptrdiff_t>(index));
    names[to].push_back(names[from][index]);
    names[from].erase(names[from].begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

}  // namespace editor::page_edits
