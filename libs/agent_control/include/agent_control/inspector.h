#ifndef AGENT_CONTROL_INSPECTOR_H_
#define AGENT_CONTROL_INSPECTOR_H_

#include "agent_control/error.h"
#include "agent_control/locator.h"

#include <QWidget>

namespace agent_control
{

struct SnapshotOptions
{
    QWidget* root = nullptr;       // nullptr => all top-level windows
    int max_depth = -1;            // -1 => unlimited
    bool interactive_only = false; // Drop pure decoration (labels, containers).
    bool include_invisible = false;
};

// Flat list of widgets, one JSON object each, rather than a nested tree.
//
// Flat is deliberate: a nested tree spends a lot of tokens on braces and forces
// the reader to track depth to know what it is looking at, while the `path`
// field already encodes the structure exactly. It also means truncating the list
// degrades gracefully instead of losing whole subtrees.
json buildSnapshot(WidgetLocator& locator, const SnapshotOptions& options);

// One widget, same shape as a snapshot row. Used by methods that act on a single
// target and want to report what they acted on.
json describeWidget(WidgetLocator& locator, QWidget* widget);

}  // namespace agent_control

#endif  // AGENT_CONTROL_INSPECTOR_H_
