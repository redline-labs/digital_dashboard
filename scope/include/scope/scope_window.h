#ifndef SCOPE_SCOPE_WINDOW_H_
#define SCOPE_SCOPE_WINDOW_H_

#include <QMainWindow>
#include <QString>

class QLabel;

namespace scope
{

// The scope's top-level window.
//
// A QMainWindow rather than a plain QWidget because panels are QDockWidgets:
// the user composes the window by docking, tabbing, splitting and floating
// them, and QMainWindow is what implements all of that. The consequence to
// remember is that every dock MUST have an objectName -- restoreState()
// silently drops any dock it cannot name, so a workspace would come back
// missing panels with nothing logged.
class ScopeWindow : public QMainWindow
{
    Q_OBJECT

  public:
    explicit ScopeWindow(QWidget* parent = nullptr);
    ~ScopeWindow() override;

    // Path the current workspace was loaded from; empty when started cold.
    const QString& workspacePath() const { return workspace_path_; }
    void setWorkspacePath(QString path) { workspace_path_ = std::move(path); }

  private:
    void buildMenuBar();

    QString workspace_path_;

    // Shown while no panels exist. Held so the docking code can hide it once
    // the first panel arrives rather than searching the widget tree for it.
    QLabel* empty_hint_ = nullptr;
};

}  // namespace scope

#endif  // SCOPE_SCOPE_WINDOW_H_
