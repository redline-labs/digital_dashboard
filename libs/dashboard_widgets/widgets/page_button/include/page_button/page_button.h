#ifndef PAGE_BUTTON_WIDGET_H_
#define PAGE_BUTTON_WIDGET_H_

#include "page_button/config.h"
#include "dashboard/widget_types.h"

#include <QWidget>

#include <memory>
#include <string_view>

namespace dashboard
{
class PageCommandSender;
}

// A touch target that changes a page_stack's page: "back to CarPlay" on a
// vehicle page, "next" on a page with no hardware button nearby.
//
// Sends over the bus rather than calling the stack directly, so the button and
// the stack need not share a window, and the command is the same one a keypad
// or a node would send.
class PageButtonWidget : public QWidget
{
    Q_OBJECT

  public:
    using config_t = PageButtonConfig_t;
    static constexpr std::string_view kFriendlyName = "Page Button";
    static constexpr widget_type_t kWidgetType = widget_type_t::page_button;

    explicit PageButtonWidget(PageButtonConfig_t cfg, QWidget* parent = nullptr);
    ~PageButtonWidget() override;
    const config_t& getConfig() const { return _cfg; }

  protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    PageButtonConfig_t _cfg;
    bool _pressed = false;
    std::unique_ptr<dashboard::PageCommandSender> _sender;
};

#endif  // PAGE_BUTTON_WIDGET_H_
