#include "page_stack/page_stack.h"

#include "dashboard/page_command_publisher.h"
#include "dashboard_pages.capnp.h"
#include "pub_sub/expression_subscriber.h"
#include "pub_sub/topic_key.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"

#include <QMetaObject>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

namespace
{

// Commands waiting for the GUI thread. A person pressing buttons never gets near
// this; a publisher stuck in a loop does, and then the oldest are dropped.
constexpr std::size_t kMaxQueuedCommands = 8;

// Fires waiting for the GUI thread, per trigger. Same reasoning.
constexpr int kMaxPendingFires = 4;

}  // namespace

// One configured trigger. Heap-allocated and never moved: the zenoh callback
// holds its address.
struct PageStackWidget::Trigger
{
    page_trigger_t cfg;

    mutable std::mutex edge_mutex;
    page_stack::TriggerEdge edge;

    std::atomic<uint64_t> fired{0};
    std::atomic<int> pending{0};
    bool valid = false;

    // Last, so it is undeclared first; its callbacks touch everything above.
    std::unique_ptr<pub_sub::ZenohExpressionSubscriber> subscriber;

    explicit Trigger(const page_trigger_t& trigger)
        : cfg(trigger)
        , edge(trigger.edge, std::chrono::milliseconds(trigger.stale_after_ms))
    {
    }
};

PageStackWidget::PageStackWidget(PageStackConfig_t cfg, QWidget* parent)
    : QWidget(parent)
    , _cfg(std::move(cfg))
{
    connect(&_state_timer, &QTimer::timeout, this, [this]() { publishState(); });
}

PageStackWidget::~PageStackWidget()
{
    // Stop the callbacks before anything they reach is torn down.
    _command_sub.reset();
    _triggers.clear();
}

PageStackPage* PageStackWidget::addPage(const std::string& name, bool in_cycle)
{
    auto* page_widget = new PageStackPage(this);
    page_widget->setGeometry(rect());
    // Explicitly hidden, so showing the stack does not show every page with it.
    page_widget->hide();
    _pages.push_back(page_widget);
    _page_info.push_back({name, in_cycle});
    return page_widget;
}

void PageStackWidget::addChild(std::size_t page_index, QWidget* child, std::size_t config_index)
{
    _children.push_back({QPointer<QWidget>(child), page_index, config_index});
}

void PageStackWidget::setPlaceholderPageNames(std::vector<std::string> names)
{
    _placeholder_names = std::move(names);
    update();
}

PageStackPage* PageStackWidget::page(std::size_t index) const
{
    return index < _pages.size() ? _pages[index] : nullptr;
}

void PageStackWidget::start(const std::string& id, std::optional<std::string> keep_page)
{
    _stack_id = id;
    _started = true;
    _navigator = std::make_unique<page_stack::PageNavigator>(_page_info, _cfg.default_page);
    if (keep_page)
    {
        _navigator->apply(page_action_t::go_to, *keep_page);
    }
    showCurrent();

    const std::string command_key = dashboard::pageCommandKey(_stack_id);
    if (!pub_sub::isValidTopicKey(command_key))
    {
        // Validation refuses this at load; only a hand-built stack gets here.
        // Local commands (pages.command, the MCP tools) still work.
        SPDLOG_ERROR("page_stack '{}': id is not usable in a topic key; not on the bus.", _stack_id);
        return;
    }

    _state_pub = std::make_unique<pub_sub::ZenohPublisher<PageStackState>>(dashboard::pageStateKey(_stack_id));

    _command_sub = std::make_unique<pub_sub::ZenohTypedSubscriber<PageStackCommand>>(
        command_key,
        [this](PageStackCommand::Reader reader)
        {
            // Zenoh thread: copy out, hand over, return.
            const page_action_t action = dashboard::fromCapnp(reader.getAction());
            std::string page_name = reader.getPage().cStr();

            bool queue_drain = false;
            {
                const std::lock_guard<std::mutex> lock(_command_mutex);
                if (_commands.size() >= kMaxQueuedCommands)
                {
                    _commands.pop_front();
                    SPDLOG_WARN("page_stack '{}': commands arriving faster than they apply; dropped one.", _stack_id);
                }
                _commands.emplace_back(action, std::move(page_name));
                queue_drain = !_drain_queued;
                _drain_queued = true;
            }
            if (queue_drain)
            {
                QMetaObject::invokeMethod(this, [this]() { drainCommands(); }, Qt::QueuedConnection);
            }
        });

    for (const page_trigger_t& trigger_cfg : _cfg.triggers)
    {
        auto trigger = std::make_unique<Trigger>(trigger_cfg);
        Trigger* raw = trigger.get();
        try
        {
            raw->subscriber = std::make_unique<pub_sub::ZenohExpressionSubscriber>(
                trigger_cfg.schema_type, trigger_cfg.expression, trigger_cfg.zenoh_key);
        }
        catch (const std::exception& e)
        {
            SPDLOG_ERROR("page_stack '{}': trigger on '{}' failed to subscribe: {}", _stack_id,
                         trigger_cfg.zenoh_key, e.what());
        }

        raw->valid = raw->subscriber && raw->subscriber->isValid();
        if (!raw->valid)
        {
            // The stack still works; this one input does not. Said once, with
            // the key, which is what a person would grep the config for.
            SPDLOG_ERROR("page_stack '{}': trigger on '{}' with expression '{}' is invalid and will never fire.",
                         _stack_id, trigger_cfg.zenoh_key, trigger_cfg.expression);
        }
        else
        {
            raw->subscriber->setResultCallback<double>(
                [this, raw](double value)
                {
                    bool fires = false;
                    {
                        const std::lock_guard<std::mutex> lock(raw->edge_mutex);
                        fires = raw->edge.onSample(value, std::chrono::steady_clock::now());
                    }
                    if (!fires)
                    {
                        return;
                    }
                    raw->fired.fetch_add(1, std::memory_order_relaxed);
                    if (raw->pending.load(std::memory_order_relaxed) >= kMaxPendingFires)
                    {
                        return;
                    }
                    raw->pending.fetch_add(1, std::memory_order_relaxed);
                    QMetaObject::invokeMethod(
                        this,
                        [this, raw]()
                        {
                            raw->pending.fetch_sub(1, std::memory_order_relaxed);
                            SPDLOG_INFO("page_stack '{}': trigger on '{}' fired: {} '{}'", _stack_id,
                                        raw->cfg.zenoh_key, reflection::enum_to_string(raw->cfg.action),
                                        raw->cfg.page);
                            apply(raw->cfg.action, raw->cfg.page);
                        },
                        Qt::QueuedConnection);
                });
        }
        _triggers.push_back(std::move(trigger));
    }

    _state_timer.start(std::chrono::seconds(1));
    publishState();
}

void PageStackWidget::drainCommands()
{
    std::deque<std::pair<page_action_t, std::string>> commands;
    {
        const std::lock_guard<std::mutex> lock(_command_mutex);
        commands.swap(_commands);
        _drain_queued = false;
    }
    for (const auto& [action, page_name] : commands)
    {
        apply(action, page_name);
    }
}

bool PageStackWidget::apply(page_action_t action, const std::string& page_name, std::string* why)
{
    if (!_navigator)
    {
        if (why) *why = "the stack has not started";
        return false;
    }

    const std::string before = currentPage();
    std::string reason;
    switch (_navigator->apply(action, page_name))
    {
        case page_stack::PageNavigator::Result::changed:
            SPDLOG_INFO("page_stack '{}': {} -> {} ({})", _stack_id, before, currentPage(),
                        reflection::enum_to_string(action));
            showCurrent();
            return true;
        case page_stack::PageNavigator::Result::unchanged:
            return true;
        case page_stack::PageNavigator::Result::unknown_page:
            reason = "no page named '" + page_name + "'";
            break;
        case page_stack::PageNavigator::Result::missing_page_name:
            reason = "go_to needs a page name";
            break;
        case page_stack::PageNavigator::Result::empty:
            reason = "the stack has no pages";
            break;
    }
    SPDLOG_WARN("page_stack '{}': {} '{}' refused: {}", _stack_id, reflection::enum_to_string(action), page_name, reason);
    if (why) *why = reason;
    return false;
}

void PageStackWidget::showCurrent()
{
    if (!_navigator || _navigator->empty())
    {
        return;
    }
    const std::size_t current = _navigator->current();
    // Hide first, then show: a widget moving between pages never sees both.
    for (std::size_t i = 0; i < _pages.size(); ++i)
    {
        if (i != current)
        {
            _pages[i]->hide();
        }
    }
    _pages[current]->show();

    emit pageChanged(QString::fromStdString(currentPage()), QString::fromStdString(previousPage()));
    publishState();
}

std::string PageStackWidget::currentPage() const
{
    if (!_navigator || _navigator->empty())
    {
        return {};
    }
    return _page_info[_navigator->current()].name;
}

std::string PageStackWidget::previousPage() const
{
    if (!_navigator || !_navigator->previous())
    {
        return {};
    }
    return _page_info[*_navigator->previous()].name;
}

void PageStackWidget::publishState()
{
    if (!_state_pub || !_navigator)
    {
        return;
    }
    auto& fields = _state_pub->fields();
    fields.setCurrent(currentPage());
    fields.setPrevious(previousPage());
    // Page counts are a handful; the config cannot reach UInt16's limit and
    // still load in any useful time.
    fields.setIndex(static_cast<uint16_t>(_navigator->current()));
    fields.setPageCount(static_cast<uint16_t>(_pages.size()));
    _state_pub->put();
}

std::optional<PageStackWidget::ChildSlot> PageStackWidget::slotOf(const QWidget* child) const
{
    for (const Child& entry : _children)
    {
        if (entry.widget == child)
        {
            return ChildSlot{entry.page, entry.config_index};
        }
    }
    return std::nullopt;
}

std::vector<QWidget*> PageStackWidget::pageChildren(std::size_t page_index) const
{
    std::vector<QWidget*> out;
    for (const Child& entry : _children)
    {
        if (entry.page == page_index && entry.widget)
        {
            out.push_back(entry.widget);
        }
    }
    return out;
}

bool PageStackWidget::replaceChild(QWidget* existing, QWidget* replacement)
{
    for (Child& entry : _children)
    {
        if (entry.widget == existing)
        {
            entry.widget = replacement;
            delete existing;
            return true;
        }
    }
    return false;
}

std::vector<PageStackWidget::TriggerInfo> PageStackWidget::triggerInfo() const
{
    std::vector<TriggerInfo> out;
    for (const auto& trigger : _triggers)
    {
        TriggerInfo info;
        info.zenoh_key = trigger->cfg.zenoh_key;
        info.valid = trigger->valid;
        {
            const std::lock_guard<std::mutex> lock(trigger->edge_mutex);
            info.primed = trigger->edge.primed();
        }
        info.fired = trigger->fired.load(std::memory_order_relaxed);
        out.push_back(info);
    }
    return out;
}

void PageStackWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    for (PageStackPage* page_widget : _pages)
    {
        page_widget->setGeometry(rect());
    }
}

void PageStackWidget::paintEvent(QPaintEvent* /*event*/)
{
    // A live stack draws nothing of its own: its pages cover it. Only the
    // editor's preview, which has no pages, shows where the stack is.
    if (!_pages.empty())
    {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor(0x88, 0x88, 0x88));
    pen.setStyle(Qt::DashLine);
    pen.setWidth(2);
    painter.setPen(pen);
    painter.drawRect(rect().adjusted(1, 1, -2, -2));

    QString names;
    for (const std::string& name : _placeholder_names)
    {
        names += (names.isEmpty() ? "" : " | ") + QString::fromStdString(name);
    }
    const QString text = names.isEmpty() ? QStringLiteral("page stack") : QStringLiteral("pages: ") + names;
    painter.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, text);
}

#include "page_stack/moc_page_stack.cpp"
