#include "scope/panel_config_dialog.h"

#include "scope/panel.h"

#include "qt_helpers/reflected_form.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace scope
{

namespace
{

// The names configured on THIS machine are the whole set of tilesets that can
// work -- a free-text field is a guessing game, and an unguessed value is a
// permanently blank map. Editable so a name configured on another machine can
// still be typed for a shared workspace.
qt_helpers::StringFieldHook tilesetPicker(const scope_settings_t& settings)
{
    return [&settings](std::string_view field_name, std::string& value, QWidget* parent,
                       std::function<void()> changed) -> QWidget*
    {
        if (field_name != "tileset")
        {
            return nullptr;
        }
        auto* combo = new QComboBox(parent);
        combo->setEditable(true);
        combo->addItem(QString());
        for (const scope_tileset_t& tileset : settings.tilesets)
        {
            combo->addItem(QString::fromStdString(tileset.name));
        }
        combo->setCurrentText(QString::fromStdString(value));
        if (settings.tilesets.empty())
        {
            combo->setToolTip(
                QObject::tr("No tilesets are configured on this machine -- File ▸ Settings…"));
        }
        QObject::connect(combo, &QComboBox::currentTextChanged, combo,
                         [&value, notify = std::move(changed)](const QString& text)
                         {
                             value = text.toStdString();
                             notify();
                         });
        return combo;
    };
}

}  // namespace

PanelConfigDialog::PanelConfigDialog(Panel& panel, const scope_settings_t& settings,
                                     QWidget* parent) :
    QDialog(parent), panel_(&panel), settings_(&settings), config_(panelConfigOf(panel))
{
    setObjectName("panel_config_dialog");
    setWindowTitle(tr("Configure %1").arg(panel.title()));
    setMinimumSize(560, 420);

    auto* layout = new QVBoxLayout(this);

    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    layout->addWidget(scroll_, 1);

    problems_ = new QLabel(this);
    problems_->setObjectName("config_dialog_problems");
    problems_->setWordWrap(true);
    problems_->setStyleSheet(QStringLiteral("QLabel { color: #C0392B; }"));
    layout->addWidget(problems_, 0);

    buttons_ = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    buttons_->setObjectName("config_dialog_buttons");
    connect(buttons_, &QDialogButtonBox::accepted, this,
            [this]()
            {
                if (applyToPanel())
                {
                    accept();
                }
            });
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons_->button(QDialogButtonBox::Apply), &QPushButton::clicked, this,
            [this]() { applyToPanel(); });
    layout->addWidget(buttons_, 0);

    rebuildForm();
}

void PanelConfigDialog::rebuildForm()
{
    // setWidget() deletes the previous form, which is safe here: nothing that
    // calls this runs inside the form.
    form_ = nullptr;
    QWidget* content = std::visit(
        [this](auto& cfg) -> QWidget*
        {
            using cfg_t = std::decay_t<decltype(cfg)>;
            if constexpr (std::is_same_v<cfg_t, std::monostate>)
            {
                return new QWidget(scroll_);
            }
            else
            {
                auto* form = new qt_helpers::ReflectedForm(cfg, scroll_, tilesetPicker(*settings_));
                connect(form, &qt_helpers::ReflectedForm::problemsChanged, this,
                        &PanelConfigDialog::showProblems);
                form_ = form;
                return form;
            }
        },
        config_);
    scroll_->setWidget(content);
    showProblems();
}

void PanelConfigDialog::showProblems()
{
    const QStringList problems = form_ != nullptr ? form_->problems() : QStringList();
    problems_->setText(problems.join(QLatin1Char('\n')));
    problems_->setVisible(!problems.isEmpty());
    // A key that breaks the rules would be refused by the workspace loader and
    // drop the binding the next time the file is opened; it does not get in.
    buttons_->button(QDialogButtonBox::Ok)->setEnabled(problems.isEmpty());
    buttons_->button(QDialogButtonBox::Apply)->setEnabled(problems.isEmpty());
}

bool PanelConfigDialog::applyToPanel()
{
    if (form_ != nullptr && !form_->problems().isEmpty())
    {
        return false;
    }
    if (!applyPanelConfig(*panel_, config_))
    {
        SPDLOG_WARN("The panel declined its own config kind; nothing applied.");
        return false;
    }

    // Re-read what the panel actually holds -- validate() may have clamped --
    // and rebuild, so the form shows the value in force rather than what was
    // typed.
    config_ = panelConfigOf(*panel_);
    rebuildForm();
    return true;
}

}  // namespace scope

#include "scope/moc_panel_config_dialog.cpp"
