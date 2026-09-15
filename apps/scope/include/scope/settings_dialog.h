#ifndef SCOPE_SETTINGS_DIALOG_H_
#define SCOPE_SETTINGS_DIALOG_H_

// The settings editor: map archives, by the name panels refer to them by.
//
// A SECOND FRONT END, never the only one. Everything it does is also reachable
// through ScopeWindow::setSettings() and `scope.settings`, which is what keeps
// the feature testable headlessly -- a modal dialog under --mcp has nobody to
// dismiss it.
//
// The status column is the point of the dialog rather than decoration. "My map
// is blank" has four different causes that look identical on screen, and three
// of them are answerable here without launching anything: the name is not
// configured, the path is wrong, or the archive covers different zooms than the
// panel is asking for. It is the same question `map_server --check` exists to
// answer.
#include "scope/settings.h"

#include <QDialog>

class QTableWidget;
class QPushButton;

namespace scope
{

class SettingsDialog : public QDialog
{
    Q_OBJECT

  public:
    explicit SettingsDialog(scope_settings_t settings, QWidget* parent = nullptr);

    // Valid after exec() returned Accepted.
    const scope_settings_t& settings() const { return settings_; }

  private:
    void addRow(const scope_tileset_t& tileset);
    void browseForRow(int row);
    // Opens the archive and reports what is in it, or why it could not be
    // opened. Called on edit rather than on a timer: opening a 400 MB SQLite
    // file is cheap, and a stale status is worse than none.
    void refreshStatus(int row);
    void collect();

    scope_settings_t settings_;
    QTableWidget* table_ = nullptr;
    QPushButton* remove_ = nullptr;
};

}  // namespace scope

#endif  // SCOPE_SETTINGS_DIALOG_H_
