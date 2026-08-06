#ifndef SCOPE_SCOPE_WINDOW_H_
#define SCOPE_SCOPE_WINDOW_H_

#include "scope/panel_registry.h"
#include "scope/panel_types.h"

#include <QMainWindow>
#include <QString>

#include <memory>
#include <vector>

class QDockWidget;
class QLabel;
class QDoubleSpinBox;
class QToolButton;

namespace scope
{

class DataSource;
class LiveZenohSource;
class Panel;
class SignalBrowser;
class TimeBase;

// The scope's top-level window.
//
// A QMainWindow rather than a plain QWidget because panels are QDockWidgets:
// the user composes the window by docking, tabbing, splitting and floating
// them, and QMainWindow is what implements all of that.
//
// The consequence to remember is that every dock MUST have an objectName.
// restoreState() silently drops any dock it cannot name, so a workspace would
// come back missing panels with nothing logged -- which is why addPanel()
// assigns one and nothing else is allowed to.
class ScopeWindow : public QMainWindow
{
    Q_OBJECT

  public:
    explicit ScopeWindow(QWidget* parent = nullptr);
    ~ScopeWindow() override;

    // A panel and the dock that holds it, paired because everything that
    // addresses a panel from outside -- the agent interface, the workspace
    // codec -- needs both.
    struct PanelEntry
    {
        QString id;
        Panel* panel = nullptr;
        QDockWidget* dock = nullptr;
    };

    // Adds a panel of `type` and returns its id, or an empty string when the
    // type is unknown. `id` is generated when empty.
    QString addPanel(panel_type_t type, const QString& id = QString());

    // Adds a panel from an existing configuration -- what loading a workspace
    // does.
    QString addPanelFromConfig(const panel_config_variant_t& config, const QString& id);

    bool removePanel(const QString& id);

    const std::vector<PanelEntry>& panels() const { return panels_; }
    PanelEntry* findPanel(const QString& id);

    TimeBase& timeBase() { return *time_base_; }
    DataSource& source();
    SignalBrowser* browser() { return browser_; }

    const QString& workspacePath() const { return workspace_path_; }
    void setWorkspacePath(QString path) { workspace_path_ = std::move(path); }

    // Dock arrangement as an opaque, Qt-versioned blob. See the comment in
    // workspace.cpp for why this is stored alongside the readable YAML rather
    // than instead of it.
    QByteArray dockState() const;
    bool restoreDockState(const QByteArray& state);

  private:
    void buildMenuBar();
    void showPanelMenu(const QString& panel_id, const QPoint& at);
    void buildTransportBar();
    void updateEmptyHint();
    QString uniqueId(panel_type_t type) const;

    std::unique_ptr<LiveZenohSource> source_;
    std::unique_ptr<TimeBase> time_base_;

    std::vector<PanelEntry> panels_;
    int next_panel_ordinal_ = 1;

    SignalBrowser* browser_ = nullptr;
    QDockWidget* browser_dock_ = nullptr;

    QLabel* empty_hint_ = nullptr;
    QToolButton* pause_button_ = nullptr;
    QDoubleSpinBox* window_spin_ = nullptr;
    QLabel* cursor_label_ = nullptr;

    QString workspace_path_;
};

}  // namespace scope

#endif  // SCOPE_SCOPE_WINDOW_H_
