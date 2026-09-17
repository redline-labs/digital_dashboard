#ifndef PAGE_STACK_WIDGET_H_
#define PAGE_STACK_WIDGET_H_

#include "page_stack/config.h"
#include "page_stack/page_navigator.h"
#include "page_stack/trigger_edge.h"

#include "dashboard/widget_types.h"

#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pub_sub
{
class ZenohExpressionSubscriber;
template <typename SchemaT>
class ZenohTypedSubscriber;
template <typename SchemaT>
class ZenohPublisher;
}  // namespace pub_sub

struct PageStackCommand;
struct PageStackState;

// One page of a page_stack: fills the stack, holds that page's widgets. A class
// of its own so a selector path reads PageStackWidget/PageStackPage[1]/..., and
// so nothing mistakes it for a configurable widget.
class PageStackPage : public QWidget
{
    Q_OBJECT

  public:
    using QWidget::QWidget;
};

// A region of the window showing one of several pages of widgets.
//
// Switching pages HIDES the others; it never destroys them. That is what keeps a
// CarPlay widget's audio playing on another page, and what keeps every hidden
// gauge's staleness current so it is right the moment it reappears.
//
// Built in two steps, because the widget libraries cannot see the layout config
// (dashboard/app_config.h includes them): the factory constructs this from its
// config_t like any widget, then dashboard::buildWidget() adds the pages and
// their widgets and calls start(). Without start() -- the editor's preview --
// there is no bus I/O and the stack draws an outline naming its pages.
class PageStackWidget : public QWidget
{
    Q_OBJECT

  public:
    using config_t = PageStackConfig_t;
    static constexpr std::string_view kFriendlyName = "Page Stack";
    static constexpr widget_type_t kWidgetType = widget_type_t::page_stack;

    PageStackWidget(PageStackConfig_t cfg, QWidget* parent = nullptr);
    ~PageStackWidget() override;
    const config_t& getConfig() const { return _cfg; }

    // ---- construction, before start()

    PageStackPage* addPage(const std::string& name, bool in_cycle);
    // `child` must already be parented to page `page`. `config_index` is its
    // position in that page's widget list, kept for rebuilds.
    void addChild(std::size_t page, QWidget* child, std::size_t config_index);

    // For the editor, which builds no pages: what to write in the outline, and
    // which page it is previewing.
    void setPlaceholderPageNames(std::vector<std::string> names, std::optional<std::size_t> shown = std::nullopt);

    // Shows the default page -- or `keep_page` when rebuilding a live stack --
    // subscribes to the command topic and the triggers, and starts publishing
    // state. `id` names the topics.
    void start(const std::string& id, std::optional<std::string> keep_page = std::nullopt);

    // ---- running

    // Applies a command on the GUI thread. On failure, `why` says what was wrong.
    bool apply(page_action_t action, const std::string& page, std::string* why = nullptr);

    std::string currentPage() const;
    std::string previousPage() const;
    const std::string& id() const { return _stack_id; }
    std::size_t pageCount() const { return _pages.size(); }
    PageStackPage* page(std::size_t index) const;
    const page_stack::PageNavigator::Page& pageInfo(std::size_t index) const { return _page_info[index]; }
    bool started() const { return _started; }

    struct ChildSlot
    {
        std::size_t page;
        std::size_t config_index;
    };
    std::optional<ChildSlot> slotOf(const QWidget* child) const;
    // Every live child of one page, in construction order.
    std::vector<QWidget*> pageChildren(std::size_t page) const;

    // Swaps `existing` for `replacement` (already parented to the same page and
    // placed) and destroys `existing` before returning, so a snapshot taken
    // straight after does not still see it.
    bool replaceChild(QWidget* existing, QWidget* replacement);

    struct TriggerInfo
    {
        std::string zenoh_key;
        bool valid = false;
        bool primed = false;
        uint64_t fired = 0;
    };
    std::vector<TriggerInfo> triggerInfo() const;

  signals:
    void pageChanged(const QString& current, const QString& previous);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    struct Trigger;

    void showCurrent();
    void publishState();
    void drainCommands();

    PageStackConfig_t _cfg;
    std::string _stack_id;
    bool _started = false;

    std::vector<PageStackPage*> _pages;  // children of this, owned by Qt
    std::vector<page_stack::PageNavigator::Page> _page_info;
    std::unique_ptr<page_stack::PageNavigator> _navigator;
    std::vector<std::string> _placeholder_names;
    std::optional<std::size_t> _placeholder_shown;

    struct Child
    {
        QPointer<QWidget> widget;
        std::size_t page;
        std::size_t config_index;
    };
    std::vector<Child> _children;

    // Commands arriving from the bus, handed to the GUI thread. Bounded: a
    // publisher stuck in a loop must not grow the event queue without limit.
    std::mutex _command_mutex;
    std::deque<std::pair<page_action_t, std::string>> _commands;
    bool _drain_queued = false;

    std::unique_ptr<pub_sub::ZenohPublisher<PageStackState>> _state_pub;
    QTimer _state_timer;

    // Declared last so they are destroyed first: undeclaring joins in-flight
    // callbacks, which reach into everything above.
    std::vector<std::unique_ptr<Trigger>> _triggers;
    std::unique_ptr<pub_sub::ZenohTypedSubscriber<PageStackCommand>> _command_sub;
};

#endif  // PAGE_STACK_WIDGET_H_
