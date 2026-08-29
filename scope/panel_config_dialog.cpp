#include "scope/panel_config_dialog.h"

#include "scope/panel.h"

#include "helpers/color.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <spdlog/spdlog.h>

#include <cstdint>
#include <string>
#include <type_traits>
#include <string_view>
#include <vector>

namespace scope
{

namespace
{

// nlohmann-style trait, local because config_json.h keeps its own private.
template <typename T>
struct is_std_vector : std::false_type
{
};
template <typename T, typename A>
struct is_std_vector<std::vector<T, A>> : std::true_type
{
};

// vector<reflected struct>, asked in ONE trait: writing the conjunction inline
// as `is_std_vector<F> && is_reflected_struct<F::value_type>` instantiates
// F::value_type even when F is a bool, which does not compile.
template <typename T>
struct is_struct_vector : std::false_type
{
};
template <typename T, typename A>
struct is_struct_vector<std::vector<T, A>> : reflection::is_reflected_struct<T>
{
};

// A human-readable name for one element of a vector<struct> list: the first
// non-empty string field, which for a trace or a row is its label or its
// expression -- what a person would call it. "entry N" when nothing is set yet.
template <typename Item>
QString elementLabel(const Item& item, std::size_t index)
{
    QString label;
    reflection::visit_fields<const Item>(
        item,
        [&label](std::string_view /*name*/, const auto& field, std::string_view /*type*/)
        {
            using F = std::decay_t<decltype(field)>;
            if constexpr (std::is_same_v<F, std::string>)
            {
                if (label.isEmpty() && !field.empty())
                {
                    label = QString::fromStdString(field);
                }
            }
        });
    return label.isEmpty() ? QObject::tr("entry %1").arg(index + 1) : label;
}

// Builds form rows for one reflected struct. The widgets write straight into
// the fields of the dialog's working copy through the references visit_fields
// hands out -- valid because the copy's address is stable (see the header) and
// because the only vector elements ever referenced belong to the currently
// selected list row, whose sub-form is rebuilt on every mutation.
struct FormBuilder
{
    const scope_settings_t* settings = nullptr;
    QWidget* parent = nullptr;

    template <typename T>
    void addStructFields(QFormLayout* form, T& object) const
    {
        reflection::visit_fields<T>(
            object,
            [this, form](std::string_view name, auto& field, std::string_view /*type*/)
            {
                using F = std::decay_t<decltype(field)>;

                const auto friendly = reflection::get_friendly_name<T>(name);
                const QString label = QString::fromUtf8(
                    friendly.empty() ? name.data() : friendly.data(),
                    static_cast<qsizetype>(friendly.empty() ? name.size() : friendly.size()));
                const auto description = reflection::get_description<T>(name);
                const QString tip = QString::fromUtf8(
                    description.data(), static_cast<qsizetype>(description.size()));

                QWidget* editor = makeEditor(name, field);
                if (editor == nullptr)
                {
                    return;
                }
                editor->setToolTip(tip);
                // The FIELD name, so a test (or ui_find) can address an editor
                // by the same name the config and the agent interface use.
                editor->setObjectName(QStringLiteral("config_field_%1")
                                          .arg(QString::fromUtf8(
                                              name.data(),
                                              static_cast<qsizetype>(name.size()))));

                if constexpr (is_struct_vector<F>::value)
                {
                    // A list of structs gets the whole row: label above, the
                    // list-plus-subform below.
                    auto* box = new QGroupBox(label, parent);
                    box->setToolTip(tip);
                    auto* inner = new QVBoxLayout(box);
                    inner->setContentsMargins(6, 6, 6, 6);
                    inner->addWidget(editor);
                    form->addRow(box);
                }
                else if constexpr (reflection::is_reflected_struct<F>::value)
                {
                    form->addRow(editor);
                }
                else
                {
                    form->addRow(label, editor);
                }
            });
    }

    template <typename F>
    QWidget* makeEditor(std::string_view name, F& field) const
    {
        if constexpr (std::is_same_v<F, bool>)
        {
            auto* box = new QCheckBox(parent);
            box->setChecked(field);
            QObject::connect(box, &QCheckBox::toggled, parent,
                             [&field](bool on) { field = on; });
            return box;
        }
        else if constexpr (std::is_same_v<F, helpers::Color>)
        {
            return makeColorEditor(field);
        }
        else if constexpr (std::is_same_v<F, std::string>)
        {
            if (name == "tileset" && settings != nullptr)
            {
                return makeTilesetEditor(field);
            }
            auto* edit = new QLineEdit(QString::fromStdString(field), parent);
            QObject::connect(edit, &QLineEdit::textChanged, parent,
                             [&field](const QString& text)
                             { field = text.toStdString(); });
            return edit;
        }
        else if constexpr (reflection::is_reflected_enum<F>::value)
        {
            auto* combo = new QComboBox(parent);
            for (const auto& value : reflection::enum_traits<F>::names())
            {
                combo->addItem(QString::fromUtf8(value.data(),
                                                 static_cast<qsizetype>(value.size())));
            }
            const std::string_view current = reflection::enum_to_string(field);
            combo->setCurrentText(
                QString::fromUtf8(current.data(), static_cast<qsizetype>(current.size())));
            QObject::connect(combo, &QComboBox::currentTextChanged, parent,
                             [&field](const QString& text)
                             {
                                 if (const auto value =
                                         reflection::enum_traits<F>::try_from_string(
                                             text.toStdString()))
                                 {
                                     field = *value;
                                 }
                             });
            return combo;
        }
        else if constexpr (std::is_floating_point_v<F>)
        {
            auto* spin = new QDoubleSpinBox(parent);
            spin->setRange(-1e12, 1e12);
            spin->setDecimals(4);
            spin->setValue(field);
            QObject::connect(spin, &QDoubleSpinBox::valueChanged, parent,
                             [&field](double value) { field = static_cast<F>(value); });
            return spin;
        }
        else if constexpr (std::is_integral_v<F>)
        {
            // A QDoubleSpinBox at zero decimals rather than QSpinBox: QSpinBox
            // is int-wide and max_buffer_bytes is a uint64.
            auto* spin = new QDoubleSpinBox(parent);
            spin->setDecimals(0);
            if constexpr (std::is_signed_v<F>)
            {
                spin->setRange(-9e15, 9e15);
            }
            else
            {
                spin->setRange(0.0, 9e15);
            }
            spin->setValue(static_cast<double>(field));
            QObject::connect(spin, &QDoubleSpinBox::valueChanged, parent,
                             [&field](double value) { field = static_cast<F>(value); });
            return spin;
        }
        else if constexpr (std::is_same_v<F, std::vector<std::string>>)
        {
            // Comma-separated in one line. The one field of this shape
            // (overlay_tilesets) is a short list of names.
            QStringList parts;
            for (const std::string& entry : field)
            {
                parts.push_back(QString::fromStdString(entry));
            }
            auto* edit = new QLineEdit(parts.join(", "), parent);
            edit->setPlaceholderText(QObject::tr("comma-separated"));
            QObject::connect(edit, &QLineEdit::textChanged, parent,
                             [&field](const QString& text)
                             {
                                 field.clear();
                                 for (const QString& part :
                                      text.split(',', Qt::SkipEmptyParts))
                                 {
                                     field.push_back(part.trimmed().toStdString());
                                 }
                             });
            return edit;
        }
        else if constexpr (is_struct_vector<F>::value)
        {
            return makeListEditor(field);
        }
        else if constexpr (reflection::is_reflected_struct<F>::value)
        {
            const auto friendly = reflection::get_friendly_name<F>(name);
            auto* box = new QGroupBox(
                QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size())), parent);
            static_cast<void>(friendly);
            auto* form = new QFormLayout(box);
            addStructFields(form, field);
            return box;
        }
        else
        {
            static_assert(sizeof(F) == 0, "panel_config_dialog: unsupported field type.");
            return nullptr;
        }
    }

    QWidget* makeColorEditor(helpers::Color& field) const
    {
        auto* row = new QWidget(parent);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);

        auto* edit = new QLineEdit(QString::fromStdString(field.value()), row);
        edit->setPlaceholderText(QStringLiteral("#RRGGBB"));
        auto* pick = new QPushButton(QObject::tr("…"), row);
        pick->setFixedWidth(28);

        const auto swatch = [edit]()
        {
            const QColor colour(edit->text());
            edit->setStyleSheet(colour.isValid()
                                    ? QStringLiteral("border-left: 6px solid %1;")
                                          .arg(colour.name())
                                    : QString());
        };
        QObject::connect(edit, &QLineEdit::textChanged, parent,
                         [&field, swatch](const QString& text)
                         {
                             field = text.toStdString();
                             swatch();
                         });
        QObject::connect(pick, &QPushButton::clicked, parent,
                         [edit]()
                         {
                             const QColor chosen = QColorDialog::getColor(
                                 QColor(edit->text()), edit->window());
                             if (chosen.isValid())
                             {
                                 edit->setText(chosen.name());
                             }
                         });
        swatch();

        layout->addWidget(edit, 1);
        layout->addWidget(pick, 0);
        return row;
    }

    QWidget* makeTilesetEditor(std::string& field) const
    {
        // The names configured on THIS machine, which is the whole set of
        // values that can work -- a free-text field here is a guessing game,
        // and an unguessed value is a permanently blank map. Editable so a
        // name configured on another machine can still be typed for a shared
        // workspace.
        auto* combo = new QComboBox(parent);
        combo->setEditable(true);
        combo->addItem(QString());
        for (const scope_tileset_t& tileset : settings->tilesets)
        {
            combo->addItem(QString::fromStdString(tileset.name));
        }
        combo->setCurrentText(QString::fromStdString(field));
        if (settings->tilesets.empty())
        {
            combo->setToolTip(QObject::tr(
                "No tilesets are configured on this machine -- File ▸ Settings…"));
        }
        QObject::connect(combo, &QComboBox::currentTextChanged, parent,
                         [&field](const QString& text) { field = text.toStdString(); });
        return combo;
    }

    // The list half of a vector<struct> field: rows on the left, the selected
    // element's own form on the right, Add/Remove underneath. The sub-form is
    // REBUILT on every selection change and every mutation, because add and
    // remove reallocate the vector and would leave the old sub-form's field
    // references dangling -- rebuilding is what keeps them fresh.
    template <typename Item>
    QWidget* makeListEditor(std::vector<Item>& items) const
    {
        auto* host = new QWidget(parent);
        auto* layout = new QHBoxLayout(host);
        layout->setContentsMargins(0, 0, 0, 0);

        auto* left = new QWidget(host);
        auto* left_layout = new QVBoxLayout(left);
        left_layout->setContentsMargins(0, 0, 0, 0);

        auto* list = new QListWidget(left);
        list->setObjectName("config_list");
        auto* buttons = new QWidget(left);
        auto* buttons_layout = new QHBoxLayout(buttons);
        buttons_layout->setContentsMargins(0, 0, 0, 0);
        auto* add = new QPushButton(QObject::tr("Add"), buttons);
        auto* remove = new QPushButton(QObject::tr("Remove"), buttons);
        buttons_layout->addWidget(add);
        buttons_layout->addWidget(remove);
        left_layout->addWidget(list, 1);
        left_layout->addWidget(buttons, 0);

        auto* element_host = new QGroupBox(host);
        element_host->setObjectName("config_list_element");

        const FormBuilder builder = *this;

        const auto refill = [list, &items]()
        {
            const int selected = list->currentRow();
            list->clear();
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                list->addItem(elementLabel(items[i], i));
            }
            if (list->count() > 0)
            {
                list->setCurrentRow(
                    std::clamp(selected, 0, list->count() - 1));
            }
        };

        const auto rebuildElement = [element_host, list, &items, builder]()
        {
            // Throw the old form away wholesale; its widgets hold references
            // into a vector that may just have reallocated.
            qDeleteAll(element_host->children());
            const int row = list->currentRow();
            if (row < 0 || static_cast<std::size_t>(row) >= items.size())
            {
                element_host->setTitle(QString());
                return;
            }
            element_host->setTitle(elementLabel(items[static_cast<std::size_t>(row)],
                                                static_cast<std::size_t>(row)));
            auto* form = new QFormLayout(element_host);
            builder.addStructFields(form, items[static_cast<std::size_t>(row)]);
        };

        QObject::connect(list, &QListWidget::currentRowChanged, parent,
                         [rebuildElement](int) { rebuildElement(); });
        QObject::connect(add, &QPushButton::clicked, parent,
                         [list, &items, refill, rebuildElement]()
                         {
                             items.push_back(Item{});
                             refill();
                             list->setCurrentRow(list->count() - 1);
                             rebuildElement();
                         });
        QObject::connect(remove, &QPushButton::clicked, parent,
                         [list, &items, refill, rebuildElement]()
                         {
                             const int row = list->currentRow();
                             if (row >= 0 && static_cast<std::size_t>(row) < items.size())
                             {
                                 items.erase(items.begin() + row);
                                 refill();
                                 rebuildElement();
                             }
                         });

        refill();
        rebuildElement();

        layout->addWidget(left, 0);
        layout->addWidget(element_host, 1);
        return host;
    }
};

}  // namespace

PanelConfigDialog::PanelConfigDialog(Panel& panel, const scope_settings_t& settings,
                                     QWidget* parent) :
    QDialog(parent), panel_(&panel), settings_(&settings), config_(panelConfigOf(panel))
{
    setObjectName("panel_config_dialog");
    setWindowTitle(tr("Configure %1").arg(panel.title()));
    setMinimumSize(560, 420);

    auto* layout = new QVBoxLayout(this);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    form_host_ = new QWidget(scroll);
    scroll->setWidget(form_host_);
    layout->addWidget(scroll, 1);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    buttons->setObjectName("config_dialog_buttons");
    connect(buttons, &QDialogButtonBox::accepted, this,
            [this]()
            {
                applyToPanel();
                accept();
            });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this,
            [this]() { applyToPanel(); });
    layout->addWidget(buttons, 0);

    rebuildForm();
}

void PanelConfigDialog::rebuildForm()
{
    qDeleteAll(form_host_->children());
    auto* form = new QFormLayout(form_host_);

    const FormBuilder builder{settings_, form_host_};
    std::visit(
        [&](auto& cfg)
        {
            using cfg_t = std::decay_t<decltype(cfg)>;
            if constexpr (!std::is_same_v<cfg_t, std::monostate>)
            {
                builder.addStructFields(form, cfg);
            }
        },
        config_);
}

void PanelConfigDialog::applyToPanel()
{
    if (!applyPanelConfig(*panel_, config_))
    {
        SPDLOG_WARN("The panel declined its own config kind; nothing applied.");
        return;
    }

    // Re-read what the panel actually holds -- validate() may have clamped --
    // and rebuild, so the form shows the value in force rather than what was
    // typed.
    config_ = panelConfigOf(*panel_);
    rebuildForm();
}

}  // namespace scope

#include "scope/moc_panel_config_dialog.cpp"
