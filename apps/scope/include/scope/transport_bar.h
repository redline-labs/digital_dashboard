#ifndef SCOPE_TRANSPORT_BAR_H_
#define SCOPE_TRANSPORT_BAR_H_

#include <vector>

class QAction;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QToolButton;

namespace scope
{

class ScopeWindow;
struct SourceCaps;

// The bottom of the window: the overview strip's toolbar and the transport
// controls under it -- pause/follow, the playback cluster, the rate combo, the
// window spin box, the cursor readout and the capture status.
//
// Split out of ScopeWindow because it is the window's largest cohesive block
// of widget state, with two rules of its own worth keeping in one file:
//
//   - CONTROLS ARE BUILT ONCE and shown or hidden from caps(). The agent
//     interface addresses them by objectName, and widgets recreated on every
//     source swap would either lose their names or accumulate duplicates.
//   - EVERY CONTROL IS WRITTEN FROM update(), reading the time base back,
//     never from its own toggled() handler -- a pan turns following off
//     without touching the button, and a button left to its own handler sits
//     there saying the wrong thing over a plot that has stopped scrolling.
//
// A friend of ScopeWindow, like OverviewController and for the same reason:
// this is the window's own machinery in a separate file, not a reusable
// widget. The widgets stay children of the window's toolbars, so findChild by
// objectName -- what the tests and the agent socket use -- is unchanged.
class TransportBar
{
  public:
    explicit TransportBar(ScopeWindow& window) : window_(window) {}

    // Create both toolbars and their widgets, and wire the time base's
    // signals. Called once from the window's constructor.
    void build();

    // Refresh every control from current state: the window span, the play
    // state, the cursor readout with the recording's wall clock, the capture
    // status line, the pause/follow label -- then the source chip and the
    // overview. Runs once per render tick and on every time-base change.
    void update();

    // The caps-dependent half of a source swap: which control set is visible,
    // and the resets that follow the time base's own (play unchecked, rate
    // back to 1x).
    void applyCaps(const SourceCaps& caps);

  private:
    ScopeWindow& window_;

    QToolButton* pause_button_ = nullptr;
    QToolButton* play_button_ = nullptr;
    QComboBox* rate_combo_ = nullptr;
    QDoubleSpinBox* window_spin_ = nullptr;
    QLabel* cursor_label_ = nullptr;
    QLabel* transport_status_ = nullptr;

    // Shown for a live source / a seekable source respectively.
    std::vector<QAction*> live_controls_;
    std::vector<QAction*> review_controls_;
};

}  // namespace scope

#endif  // SCOPE_TRANSPORT_BAR_H_
