#ifndef AGENT_CONTROL_LOCATOR_H_
#define AGENT_CONTROL_LOCATOR_H_

#include "agent_control/error.h"

#include <QPointer>
#include <QString>
#include <QWidget>

#include <cstdint>
#include <string>
#include <vector>

namespace agent_control
{

// Resolves the selector strings the agent uses into live widgets, and hands out
// the short refs that make repeated addressing cheap.
//
// Selector forms, tried in this order:
//   w7                            an opaque ref from a previous ui.snapshot
//   #speedo                       objectName exact match
//   mercedes_190e_speedometer#0   also an objectName -- the derived fallback
//                                 name has this shape, so no special case
//   MainWindow/CarPlayWidget[0]   path of class names from a top-level window
//   CarPlayWidget                 class or objectName, searched anywhere
//
// A selector that matches nothing, or more than one widget, is ALWAYS an error.
// Silently taking the first match is how an agent ends up confidently driving
// the wrong widget and reporting a wrong conclusion, which is worse than failing.
class WidgetLocator
{
  public:
    // Defaults to QApplication::topLevelWidgets() when left empty.
    void setRoots(std::vector<QWidget*> roots);
    std::vector<QWidget*> roots() const;

    Result<QWidget*> resolve(const std::string& selector);

    // Canonical path, e.g. "MainWindow/CarPlayWidget[0]". Unique by
    // construction: the [n] suffix appears whenever a widget has siblings of the
    // same class.
    static QString pathOf(const QWidget* widget);

    // Stable across snapshots: asking twice for the same widget returns the same
    // ref, so an agent can keep using refs from an older snapshot as long as the
    // widgets are alive.
    QString refFor(QWidget* widget);
    Result<QWidget*> widgetForRef(const std::string& ref) const;

    // Bumped when a snapshot observes a structurally different tree. Returned
    // alongside snapshots and screenshots so a caller can tell that what it is
    // looking at has changed underneath it.
    std::uint64_t revision() const { return revision_; }

    // Recomputes the structural signature and bumps the revision if it moved.
    void noteTreeState(const std::vector<QWidget*>& widgets);

    // Every widget under the configured roots, parents before children.
    std::vector<QWidget*> allWidgets() const;

  private:
    Result<QWidget*> resolvePath(const std::string& selector);
    std::vector<QWidget*> matchAnywhere(const QString& token) const;
    json describeCandidates(const std::vector<QWidget*>& widgets) const;

    std::vector<QPointer<QWidget>> roots_;
    std::vector<QPointer<QWidget>> refs_;
    std::uint64_t revision_ = 1;
    std::size_t signature_ = 0;
};

}  // namespace agent_control

#endif  // AGENT_CONTROL_LOCATOR_H_
