#ifndef QT_HELPERS_REFLECTED_FORM_H_
#define QT_HELPERS_REFLECTED_FORM_H_

#include <QCheckBox>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>

#include "helpers/color.h"
#include "helpers/string_leaf.h"
#include "reflection/reflection.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace qt_helpers
{

// A field the caller renders itself, offered before the built-in editor for a
// std::string field: scope's tileset picker. Return nullptr to decline.
// `changed` must be called after writing `value`.
using StringFieldHook = std::function<QWidget*(std::string_view field_name, std::string& value,
                                               QWidget* parent, std::function<void()> changed)>;

template <typename Root>
struct ReflectedFormBuilder;

// A form over a reflected struct that writes every edit straight into it.
//
// The editor's properties panel and scope's panel dialog each had their own
// builder, and both had the same integer bug in different shapes: an unsigned
// field ranged to INT_MAX and read back with static_cast (a uint16_t of 70000
// saved as 4464), or ranged to 9e15 through a double (undefined behaviour for
// anything out of range). One builder, with every spin box ranged to the field's
// own type, is the fix for both.
//
// Live binding: `config` must outlive the form and stay at its address. A
// list's rows are rebuilt on every add and remove, because those reallocate the
// vector and would leave the old rows' references dangling.
//
// Editors are named `field:<path>` -- `field:rpm.zenoh_key`, `field:colors[2]`
// -- so a test or the agent interface can address one. A field with rules of its
// own (a topic or service key) turns red with the reason as its tooltip while
// its text breaks them; problems() lists those, and a caller blocks Apply on it.
class ReflectedForm : public QWidget
{
    Q_OBJECT

  public:
    template <typename T>
    ReflectedForm(T& config, QWidget* parent = nullptr, StringFieldHook string_hook = {});

    // "path: reason" for each field currently breaking its rules.
    QStringList problems() const;

    // The property a rule-checked line edit carries its problem in; empty when
    // the text is acceptable.
    static constexpr const char* kProblemProperty = "redlineFieldProblem";

  signals:
    // Any field was written.
    void edited();
    // A rule-checked field's problem appeared, changed or cleared.
    void problemsChanged();

  private:
    template <typename T>
    friend struct ReflectedFormBuilder;

    void notifyEdited() { emit edited(); }
    void notifyProblems() { emit problemsChanged(); }
};

// The machinery behind ReflectedForm, in the header because it is templated on
// every field type. Not for use on its own.
template <typename Root>
struct ReflectedFormBuilder
{
    ReflectedForm* owner;
    StringFieldHook string_hook;

    void changed() const { owner->notifyEdited(); }

    static QString fieldPath(const QString& parent, std::string_view name)
    {
        const QString leaf = QString::fromUtf8(name.data(), static_cast<qsizetype>(name.size()));
        return parent.isEmpty() ? leaf : parent + QLatin1Char('.') + leaf;
    }

    static void name(QWidget* editor, const QString& path)
    {
        editor->setObjectName(QStringLiteral("field:%1").arg(path));
    }

    // A spin box insisting on room for its widest value made the editor panel
    // wider than the panel; these may grow, but not demand it.
    static void constrain(QWidget* editor)
    {
        editor->setMinimumWidth(60);
        if (auto* combo = qobject_cast<QComboBox*>(editor))
        {
            combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            combo->setMinimumContentsLength(6);
        }
    }

    // The label for one field of `Struct`: its friendly name, and an info mark
    // carrying the description when there is one.
    template <typename Struct>
    static QWidget* label(QWidget* parent, std::string_view field_name)
    {
        // A config needs labels because it is RENDERED, so this is where the
        // requirement is checked: a field without a friendly name falls back to
        // its raw name and silently looks fine.
        static_assert(reflection::metadata_covers_all_fields<Struct>(),
                      "A config rendered in a ReflectedForm has a field with no friendly name.");
        static_assert(reflection::metadata_has_no_orphan_entries<Struct>(),
                      "A metadata entry names a field this struct does not have.");

        const std::string_view friendly = reflection::get_friendly_name<Struct>(field_name);
        const QString text = QString::fromUtf8(friendly.data(), static_cast<qsizetype>(friendly.size()));
        const std::string_view description = reflection::get_description<Struct>(field_name);

        auto* container = new QWidget(parent);
        auto* layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        auto* text_label = new QLabel(text, container);
        text_label->setStyleSheet(QStringLiteral("QLabel { font-weight: 600; }"));
        layout->addWidget(text_label);
        if (!description.empty())
        {
            auto* info = new QLabel(QStringLiteral("ⓘ"), container);
            info->setStyleSheet(QStringLiteral("QLabel { color: #0066cc; font-size: 12px; }"));
            info->setToolTip(QString::fromUtf8(description.data(), static_cast<qsizetype>(description.size())));
            layout->addWidget(info);
        }
        layout->addStretch();
        return container;
    }

    // Labels above their editors, so every editor gets the full width of a
    // narrow panel rather than what a label column leaves.
    static QFormLayout* makeForm(QWidget* parent)
    {
        auto* form = new QFormLayout(parent);
        form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setFormAlignment(Qt::AlignTop);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        form->setVerticalSpacing(6);
        return form;
    }

    template <typename Struct>
    void addFields(QFormLayout* form, QWidget* parent, Struct& object, const QString& path) const
    {
        reflection::visit_fields(object, [&](std::string_view field_name, auto& field, std::string_view) {
            QWidget* editor = editorFor(parent, field_name, field, fieldPath(path, field_name));
            form->addRow(label<Struct>(parent, field_name), editor);
        });
    }

    template <typename F>
    QWidget* editorFor(QWidget* parent, std::string_view field_name, F& field, const QString& path) const
    {
        if constexpr (reflection::is_std_vector<F>::value)
        {
            using Item = typename reflection::is_std_vector<F>::value_type;
            if constexpr (reflection::is_reflected_struct_v<Item>)
            {
                return structListEditor(parent, field, path);
            }
            else
            {
                return leafListEditor(parent, field_name, field, path);
            }
        }
        else if constexpr (reflection::is_reflected_struct_v<F>)
        {
            auto* frame = new QFrame(parent);
            frame->setObjectName(QStringLiteral("insetStructFrame"));
            frame->setFrameShape(QFrame::StyledPanel);
            frame->setStyleSheet(
                QStringLiteral("#insetStructFrame { border: 1px solid palette(mid); border-radius: 4px; }"));
            QFormLayout* form = makeForm(frame);
            form->setContentsMargins(8, 8, 8, 8);
            addFields(form, frame, field, path);
            return frame;
        }
        else
        {
            return leafEditor(parent, field_name, field, path);
        }
    }

    template <typename F>
    QWidget* leafEditor(QWidget* parent, std::string_view field_name, F& field, const QString& path) const
    {
        QWidget* editor = makeLeaf(parent, field_name, field, path);
        constrain(editor);
        return editor;
    }

    template <typename F>
    QWidget* makeLeaf(QWidget* parent, std::string_view field_name, F& field, const QString& path) const
    {
        if constexpr (std::is_same_v<F, bool>)
        {
            auto* box = new QCheckBox(parent);
            box->setChecked(field);
            name(box, path);
            QObject::connect(box, &QCheckBox::toggled, box, [self = *this, &field](bool on) {
                field = on;
                self.changed();
            });
            return box;
        }
        else if constexpr (std::is_same_v<F, helpers::Color>)
        {
            return colorEditor(parent, field, path);
        }
        else if constexpr (helpers::StringLeaf<F>)
        {
            return ruledEditor(parent, field, path);
        }
        else if constexpr (std::is_same_v<F, std::string>)
        {
            if (string_hook)
            {
                if (QWidget* own = string_hook(field_name, field, parent, [self = *this] { self.changed(); }))
                {
                    name(own, path);
                    return own;
                }
            }
            auto* edit = new QLineEdit(QString::fromStdString(field), parent);
            name(edit, path);
            QObject::connect(edit, &QLineEdit::textChanged, edit, [self = *this, &field](const QString& text) {
                field = text.toStdString();
                self.changed();
            });
            return edit;
        }
        else if constexpr (reflection::is_reflected_enum_v<F>)
        {
            auto* combo = new QComboBox(parent);
            for (const std::string_view value : reflection::enum_traits<F>::names())
            {
                combo->addItem(QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())));
            }
            const std::string_view current = reflection::enum_to_string(field);
            combo->setCurrentText(QString::fromUtf8(current.data(), static_cast<qsizetype>(current.size())));
            name(combo, path);
            QObject::connect(combo, &QComboBox::currentTextChanged, combo, [self = *this, &field](const QString& text) {
                // Non-throwing: the combo is filled from the enum, so a miss
                // means a stale form; keep the value rather than invent one.
                if (const auto value = reflection::enum_traits<F>::try_from_string(text.toStdString()))
                {
                    field = *value;
                    self.changed();
                }
            });
            return combo;
        }
        else if constexpr (std::is_integral_v<F>)
        {
            return integerEditor(parent, field, path);
        }
        else if constexpr (std::is_floating_point_v<F>)
        {
            auto* spin = new QDoubleSpinBox(parent);
            spin->setDecimals(4);
            spin->setRange(static_cast<double>(std::numeric_limits<F>::lowest()),
                           static_cast<double>(std::numeric_limits<F>::max()));
            spin->setValue(static_cast<double>(field));
            name(spin, path);
            QObject::connect(spin, &QDoubleSpinBox::valueChanged, spin, [self = *this, &field](double value) {
                field = static_cast<F>(value);
                self.changed();
            });
            return spin;
        }
        else
        {
            static_assert(sizeof(F) == 0, "ReflectedForm cannot build an editor for this field type.");
            return nullptr;
        }
    }

    // Ranged to the field's own type, so no value the box accepts can wrap or
    // overflow on the way into the field. A QSpinBox is int-wide; anything wider
    // goes through a zero-decimal QDoubleSpinBox, whose double is exact up to
    // 2^53 -- the range is cut there rather than at the type's limit.
    template <typename F>
    QWidget* integerEditor(QWidget* parent, F& field, const QString& path) const
    {
        constexpr bool fits_int =
            std::in_range<int>(std::numeric_limits<F>::min()) && std::in_range<int>(std::numeric_limits<F>::max());
        if constexpr (fits_int)
        {
            auto* spin = new QSpinBox(parent);
            spin->setRange(static_cast<int>(std::numeric_limits<F>::min()),
                           static_cast<int>(std::numeric_limits<F>::max()));
            spin->setValue(static_cast<int>(field));
            name(spin, path);
            QObject::connect(spin, &QSpinBox::valueChanged, spin, [self = *this, &field](int value) {
                // In range by construction: the box's range is the type's.
                field = static_cast<F>(value);
                self.changed();
            });
            return spin;
        }
        else
        {
            constexpr double kExact = 9007199254740992.0;  // 2^53
            const double lo = std::max(static_cast<double>(std::numeric_limits<F>::min()), -kExact);
            const double hi = std::min(static_cast<double>(std::numeric_limits<F>::max()), kExact);
            auto* spin = new QDoubleSpinBox(parent);
            spin->setDecimals(0);
            spin->setRange(lo, hi);
            spin->setValue(static_cast<double>(field));
            name(spin, path);
            QObject::connect(spin, &QDoubleSpinBox::valueChanged, spin, [self = *this, &field, lo, hi](double value) {
                // Clamped and rounded before the conversion: a double outside
                // the target type's range converts with undefined behaviour.
                field = static_cast<F>(std::llround(std::clamp(value, lo, hi)));
                self.changed();
            });
            return spin;
        }
    }

    // A string with rules of its own -- a topic or service key.
    template <typename F>
    QWidget* ruledEditor(QWidget* parent, F& field, const QString& path) const
    {
        auto* edit = new QLineEdit(QString::fromStdString(field.str()), parent);
        edit->setPlaceholderText(
            QString::fromUtf8(F::kFormatHint.data(), static_cast<qsizetype>(F::kFormatHint.size())));
        name(edit, path);
        // The character goes in and the field turns red, rather than a validator
        // swallowing the key and leaving someone wondering why their keyboard
        // stopped working.
        const auto recheck = [edit](const QString& text) {
            const std::string problem = F::problem(text.toStdString());
            edit->setProperty(ReflectedForm::kProblemProperty, QString::fromStdString(problem));
            edit->setStyleSheet(problem.empty() ? QString()
                                                : QStringLiteral("border: 1px solid #C0392B; background: #2B1A18;"));
            edit->setToolTip(QString::fromStdString(problem));
        };
        recheck(edit->text());
        QObject::connect(edit, &QLineEdit::textChanged, edit, [self = *this, &field, recheck](const QString& text) {
            field = F{text.toStdString()};
            recheck(text);
            self.changed();
            self.owner->notifyProblems();
        });
        return edit;
    }

    QWidget* colorEditor(QWidget* parent, helpers::Color& field, const QString& path) const
    {
        auto* row = new QWidget(parent);
        auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);
        auto* edit = new QLineEdit(QString::fromStdString(field.value()), row);
        edit->setPlaceholderText(QStringLiteral("#RRGGBB"));
        name(edit, path);
        auto* pick = new QPushButton(QStringLiteral("…"), row);
        pick->setFixedWidth(28);
        pick->setToolTip(QObject::tr("Choose colour"));
        name(row, path + QStringLiteral(":row"));

        // The picker button doubles as the swatch.
        const auto swatch = [edit, pick]() {
            const QColor colour(edit->text());
            pick->setStyleSheet(colour.isValid() ? QStringLiteral("background-color: %1;").arg(colour.name())
                                                 : QString());
        };
        QObject::connect(edit, &QLineEdit::textChanged, edit, [self = *this, &field, swatch](const QString& text) {
            field = helpers::Color(text.toStdString());
            swatch();
            self.changed();
        });
        QObject::connect(pick, &QPushButton::clicked, pick, [edit]() {
            const QColor chosen = QColorDialog::getColor(QColor(edit->text()), edit->window());
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

    // A list of plain values: one editor per row, each with its own remove
    // button, and Add underneath. Rebuilt on every add and remove.
    template <typename Item>
    QWidget* leafListEditor(QWidget* parent, std::string_view field_name, std::vector<Item>& items,
                            const QString& path) const
    {
        auto* host = new QWidget(parent);
        auto* outer = new QVBoxLayout(host);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(4);
        auto* rows = new QWidget(host);
        auto* rows_layout = new QVBoxLayout(rows);
        rows_layout->setContentsMargins(0, 0, 0, 0);
        rows_layout->setSpacing(4);
        auto* add = new QPushButton(QObject::tr("Add"), host);
        add->setToolTip(QObject::tr("Add an entry to the end of the list"));
        name(add, path + QStringLiteral(":add"));
        outer->addWidget(rows);
        outer->addWidget(add, 0, Qt::AlignRight);

        // Held by a shared function so a row's remove button can rebuild the
        // list it sits in.
        auto rebuild = std::make_shared<std::function<void()>>();
        // Weak inside itself, or the function would own itself and never go.
        const std::weak_ptr<std::function<void()>> weak = rebuild;
        *rebuild = [self = *this, rows, rows_layout, &items, field_name, path, weak]() {
            const std::shared_ptr<std::function<void()>> again = weak.lock();
            while (QLayoutItem* item = rows_layout->takeAt(0))
            {
                if (QWidget* widget = item->widget())
                {
                    widget->hide();
                    widget->deleteLater();
                }
                delete item;
            }
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                auto* row = new QWidget(rows);
                auto* h = new QHBoxLayout(row);
                h->setContentsMargins(0, 0, 0, 0);
                h->setSpacing(4);
                const QString item_path = QStringLiteral("%1[%2]").arg(path).arg(i);
                h->addWidget(self.leafEditor(row, field_name, items[i], item_path), 1);
                auto* remove = new QPushButton(QStringLiteral("✕"), row);
                remove->setToolTip(QObject::tr("Remove this entry"));
                remove->setFixedWidth(24);
                remove->setFlat(true);
                name(remove, item_path + QStringLiteral(":remove"));
                // Queued: the rebuild deletes this button, which must not
                // happen inside its own clicked() signal.
                QObject::connect(remove, &QPushButton::clicked, rows, [self, &items, i, again]() {
                    if (i < items.size())
                    {
                        items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
                        self.changed();
                        QMetaObject::invokeMethod(self.owner, [again]() { (*again)(); }, Qt::QueuedConnection);
                    }
                });
                h->addWidget(remove, 0);
                rows_layout->addWidget(row);
            }
        };
        QObject::connect(add, &QPushButton::clicked, host, [self = *this, &items, rebuild]() {
            items.push_back(Item{});
            self.changed();
            (*rebuild)();
        });
        (*rebuild)();
        return host;
    }

    // A human-readable name for one element of a list of structs: its label
    // when it has one, else the first non-empty string field, else "entry N".
    template <typename Item>
    static QString elementLabel(const Item& item, std::size_t index)
    {
        QString label;
        QString fallback;
        reflection::visit_fields(item, [&](std::string_view field_name, const auto& field, std::string_view) {
            using F = std::decay_t<decltype(field)>;
            if constexpr (std::is_same_v<F, std::string>)
            {
                if (!field.empty() && field_name == "label")
                {
                    label = QString::fromStdString(field);
                }
                else if (!field.empty() && fallback.isEmpty())
                {
                    fallback = QString::fromStdString(field);
                }
            }
            else if constexpr (reflection::is_reflected_struct_v<F>)
            {
                // A subscription names its signal by its expression.
                reflection::visit_fields(field, [&](std::string_view inner, const auto& value, std::string_view) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, std::string>)
                    {
                        if (inner == "expression" && !value.empty() && fallback.isEmpty())
                        {
                            fallback = QString::fromStdString(value);
                        }
                    }
                });
            }
        });
        if (!label.isEmpty())
        {
            return label;
        }
        return fallback.isEmpty() ? QObject::tr("entry %1").arg(index + 1) : fallback;
    }

    // A list of structs: rows on the left, the selected element's own form on
    // the right, Add and Remove underneath. The element form is rebuilt on every
    // selection change and every mutation.
    template <typename Item>
    QWidget* structListEditor(QWidget* parent, std::vector<Item>& items, const QString& path) const
    {
        auto* host = new QWidget(parent);
        auto* layout = new QHBoxLayout(host);
        layout->setContentsMargins(0, 0, 0, 0);

        auto* left = new QWidget(host);
        auto* left_layout = new QVBoxLayout(left);
        left_layout->setContentsMargins(0, 0, 0, 0);
        auto* list = new QListWidget(left);
        name(list, path + QStringLiteral(":list"));
        list->setMaximumHeight(220);
        list->setMaximumWidth(200);
        auto* add = new QPushButton(QObject::tr("Add"), left);
        name(add, path + QStringLiteral(":add"));
        auto* remove = new QPushButton(QObject::tr("Remove"), left);
        name(remove, path + QStringLiteral(":remove"));
        auto* buttons = new QHBoxLayout();
        buttons->addWidget(add);
        buttons->addWidget(remove);
        left_layout->addWidget(list, 1);
        left_layout->addLayout(buttons);

        auto* element = new QGroupBox(host);
        name(element, path + QStringLiteral(":element"));

        const auto refill = [list, &items]() {
            const int selected = list->currentRow();
            const QSignalBlocker block(list);
            list->clear();
            for (std::size_t i = 0; i < items.size(); ++i)
            {
                list->addItem(elementLabel(items[i], i));
            }
            if (list->count() > 0)
            {
                list->setCurrentRow(std::clamp(selected, 0, list->count() - 1));
            }
        };
        const auto rebuildElement = [self = *this, element, list, &items, path]() {
            // Wholesale: the old form's editors hold references into a vector
            // that may just have reallocated.
            for (QObject* child : element->children())
            {
                if (auto* widget = qobject_cast<QWidget*>(child))
                {
                    widget->hide();
                    widget->deleteLater();
                }
            }
            delete element->layout();
            const int row = list->currentRow();
            if (row < 0 || static_cast<std::size_t>(row) >= items.size())
            {
                element->setTitle(QString());
                return;
            }
            const auto index = static_cast<std::size_t>(row);
            element->setTitle(elementLabel(items[index], index));
            QFormLayout* form_layout = makeForm(element);
            self.addFields(form_layout, element, items[index], QStringLiteral("%1[%2]").arg(path).arg(index));
        };

        QObject::connect(list, &QListWidget::currentRowChanged, host, [rebuildElement](int) { rebuildElement(); });
        QObject::connect(add, &QPushButton::clicked, host, [self = *this, list, &items, refill, rebuildElement]() {
            items.push_back(Item{});
            self.changed();
            refill();
            list->setCurrentRow(list->count() - 1);
            rebuildElement();
        });
        QObject::connect(remove, &QPushButton::clicked, host, [self = *this, list, &items, refill, rebuildElement]() {
            const int row = list->currentRow();
            if (row >= 0 && static_cast<std::size_t>(row) < items.size())
            {
                items.erase(items.begin() + row);
                self.changed();
                refill();
                rebuildElement();
            }
        });
        refill();
        rebuildElement();

        layout->addWidget(left, 0);
        layout->addWidget(element, 1);
        return host;
    }
};

template <typename T>
ReflectedForm::ReflectedForm(T& config, QWidget* parent, StringFieldHook string_hook) : QWidget(parent)
{
    static_assert(reflection::is_reflected_struct_v<T>, "ReflectedForm edits a REFLECT_STRUCT");
    // The editors' callbacks hold copies of the builder (a pointer to this form
    // and the hook), so nothing they reach can be gone before they are.
    const ReflectedFormBuilder<T> builder{this, std::move(string_hook)};
    QFormLayout* form = ReflectedFormBuilder<T>::makeForm(this);
    form->setContentsMargins(10, 8, 10, 8);
    form->setVerticalSpacing(10);
    builder.addFields(form, this, config, QString());
}

}  // namespace qt_helpers

#endif  // QT_HELPERS_REFLECTED_FORM_H_
