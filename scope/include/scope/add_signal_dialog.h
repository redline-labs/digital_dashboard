#ifndef SCOPE_ADD_SIGNAL_DIALOG_H_
#define SCOPE_ADD_SIGNAL_DIALOG_H_

#include "scope/panel.h"

#include <QDialog>

namespace scope
{

class DataSource;
class SignalBrowser;

// "Add signal..." -- the same topic/field tree the docked browser shows, as a
// modal.
//
// Drag is the fast gesture but it is not the only one that should work. It is
// awkward when the browser dock is hidden or the panel is a floating window on
// another screen, it is inaccessible without a mouse, and QDrag::exec() cannot
// be driven by the agent interface at all. So a panel offers this too, and it
// reuses SignalBrowser rather than reimplementing the tree -- two views of what
// is on the bus that could disagree would be worse than either alone.
//
// Only candidates the target panel will accept can be chosen: the OK button
// stays disabled otherwise, which answers "why can't I add this?" before the
// question is asked.
class AddSignalDialog : public QDialog
{
    Q_OBJECT

  public:
    AddSignalDialog(DataSource& source, const Panel& target, QWidget* parent = nullptr);
    ~AddSignalDialog() override;

    // Valid only after exec() returned Accepted.
    const BindingCandidate& selected() const { return selected_; }

  private:
    const Panel& target_;
    SignalBrowser* browser_ = nullptr;
    BindingCandidate selected_;
};

}  // namespace scope

#endif  // SCOPE_ADD_SIGNAL_DIALOG_H_
