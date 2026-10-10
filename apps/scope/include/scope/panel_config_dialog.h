#ifndef SCOPE_PANEL_CONFIG_DIALOG_H_
#define SCOPE_PANEL_CONFIG_DIALOG_H_

#include "scope/panel_registry.h"
#include "scope/settings.h"

#include <QDialog>

class QDialogButtonBox;
class QLabel;
class QScrollArea;

namespace qt_helpers
{
class ReflectedForm;
}

namespace scope
{

// A reflection-driven editor for any panel's configuration.
//
// WHY GENERIC. Every panel config is a REFLECT_STRUCT with human labels and
// descriptions already written for scope.panel_describe_config -- and until
// this dialog existed, that metadata served agents only. A human could not set
// right_axis (the docs' own motivating example), a trace colour, a table
// format, or the map panel's tileset without hand-editing YAML or driving the
// agent socket; a map panel added from the GUI was a permanent dead end,
// captioned "not configured" by a Settings dialog that could not configure it.
// One form built from the same reflection covers every panel type, including
// the next one, with no per-panel UI code -- qt_helpers::ReflectedForm, the
// one the editor's properties panel uses too. A topic key that breaks the
// rules disables OK and Apply.
//
// The form edits a COPY, applied through applyPanelConfig() -- the same
// clamped path scope.panel_set_config takes, so the rebind-only-what-changed
// rule holds: editing a colour never discards a trace's history. After an
// apply the copy is refreshed from the panel, so a clamped value shows its
// clamped self rather than what was typed.
//
// `settings` is read for one field: a "tileset" string renders as a combo of
// the machine's configured tileset names instead of a free-text field nobody
// could guess a valid value for.
//
// Modal. Headless it is reached through the panel's context menu, and the agent
// interface drives it like any other widget while it is open.
class PanelConfigDialog : public QDialog
{
    Q_OBJECT

  public:
    PanelConfigDialog(Panel& panel, const scope_settings_t& settings,
                      QWidget* parent = nullptr);

  private:
    void rebuildForm();
    void showProblems();
    bool applyToPanel();

    Panel* panel_;
    const scope_settings_t* settings_;

    // The working copy every widget writes into. Its address (and therefore
    // every field reference the widgets hold) is stable for the life of the
    // dialog: the variant never changes alternative after construction.
    panel_config_variant_t config_;

    QScrollArea* scroll_ = nullptr;
    qt_helpers::ReflectedForm* form_ = nullptr;
    QLabel* problems_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
};

}  // namespace scope

#endif  // SCOPE_PANEL_CONFIG_DIALOG_H_
