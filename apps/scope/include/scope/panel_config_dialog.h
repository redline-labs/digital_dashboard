#ifndef SCOPE_PANEL_CONFIG_DIALOG_H_
#define SCOPE_PANEL_CONFIG_DIALOG_H_

#include "scope/panel_registry.h"
#include "scope/settings.h"

#include <QDialog>

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
// the next one, with no per-panel UI code.
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
// Modal, so it must not be raised headlessly -- the callers guard, the same
// rule every dialog in this window follows.
class PanelConfigDialog : public QDialog
{
    Q_OBJECT

  public:
    PanelConfigDialog(Panel& panel, const scope_settings_t& settings,
                      QWidget* parent = nullptr);

  private:
    void rebuildForm();
    void applyToPanel();

    Panel* panel_;
    const scope_settings_t* settings_;

    // The working copy every widget writes into. Its address (and therefore
    // every field reference the widgets hold) is stable for the life of the
    // dialog: the variant never changes alternative after construction.
    panel_config_variant_t config_;

    QWidget* form_host_ = nullptr;
};

}  // namespace scope

#endif  // SCOPE_PANEL_CONFIG_DIALOG_H_
