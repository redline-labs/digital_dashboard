#include "dashboard/widget_tree.h"

#include "dashboard/widget_factory.h"
#include "dashboard/widget_identity.h"
#include "dashboard/widget_registry.h"

#include <spdlog/spdlog.h>

namespace dashboard
{

QWidget* buildPageChild(const widget_config_t& cfg, const QString& object_name, QWidget* page)
{
    if (cfg.type == widget_type_t::page_stack)
    {
        SPDLOG_ERROR("'{}': a page_stack cannot be on another page_stack's page; not built.",
                     object_name.toStdString());
        return nullptr;
    }

    QWidget* child = widget_factory::createWidgetFromConfig(cfg, page);
    if (child == nullptr)
    {
        SPDLOG_ERROR("Failed to create widget of type '{}' ('{}') on a page.", reflection::enum_to_string(cfg.type),
                     object_name.toStdString());
        return nullptr;
    }
    child->setObjectName(object_name);
    child->setGeometry(cfg.x, cfg.y, cfg.width, cfg.height);
    // Explicitly shown, so it appears whenever its page does -- and only then.
    child->show();
    return child;
}

BuildResult buildWidget(const widget_config_t& cfg, const QString& object_name, QWidget* parent,
                        std::optional<std::string> keep_page)
{
    BuildResult result;
    result.widget = widget_factory::createWidgetFromConfig(cfg, parent);
    if (result.widget == nullptr)
    {
        return result;
    }
    result.widget->setObjectName(object_name);

    auto* stack = qobject_cast<PageStackWidget*>(result.widget);
    if (stack == nullptr)
    {
        return result;
    }

    // Sized now, so each page is created at the stack's size; the caller's
    // setGeometry afterwards resizes them again through resizeEvent.
    stack->resize(cfg.width, cfg.height);

    for (std::size_t p = 0; p < cfg.pages.size(); ++p)
    {
        const widget_page_t& page_cfg = cfg.pages[p];
        PageStackPage* page = stack->addPage(page_cfg.name, page_cfg.in_cycle);
        page->setObjectName(pageObjectName(object_name, page_cfg.name));

        for (std::size_t w = 0; w < page_cfg.widgets.size(); ++w)
        {
            const widget_config_t& child_cfg = page_cfg.widgets[w];
            QWidget* child =
                buildPageChild(child_cfg, childWidgetObjectName(object_name, page_cfg.name, child_cfg, w), page);
            if (child == nullptr)
            {
                ++result.child_failures;
                continue;
            }
            stack->addChild(p, child, w);
        }
    }

    // The id names the topics. Validation requires one; the object name is the
    // same string whenever it is set, and a fallback otherwise.
    stack->start(cfg.id.empty() ? object_name.toStdString() : cfg.id, std::move(keep_page));
    return result;
}

}  // namespace dashboard
