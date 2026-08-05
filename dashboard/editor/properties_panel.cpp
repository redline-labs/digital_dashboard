#include "editor/properties_panel.h"
#include "editor/widget_registry.h"
#include "editor/canvas.h"
#include "editor/selection_frame.h"
#include "editor/editor_constants.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QStackedWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QCheckBox>
#include <QFrame>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QColorDialog>

#include <limits>

#include "reflection/reflection.h"
#include "helpers/color.h"
#include "spdlog/spdlog.h"

#include <string>
#include <vector>

PropertiesPanel::PropertiesPanel(QWidget* parent):
  QWidget(parent),
  selected_(nullptr),
  stack_(new QStackedWidget(this)),
  currentPage_(nullptr),
  windowPage_(nullptr),
  winNameEdit_(nullptr),
  winWidthSpin_(nullptr),
  winHeightSpin_(nullptr),
  winBgColorEdit_(nullptr),
  canvas_(nullptr),
  isSyncing_(false)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    // Says what is being edited, not just that something is. The panel used to
    // be headed "Properties" whatever was selected, so with two widgets of the
    // same type on the canvas there was nothing on screen to tell you which
    // one's values you were looking at.
    heading_ = new QLabel(this);
    heading_->setStyleSheet("QLabel { font-weight: 700; font-size: 13px; }");
    subheading_ = new QLabel(this);
    subheading_->setStyleSheet("QLabel { color: palette(mid); font-size: 11px; }");
    subheading_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    layout->addWidget(heading_);
    layout->addWidget(subheading_);
    layout->addWidget(stack_);
    layout->addStretch();
    setLayout(layout);

    showHeading(nullptr);

    // Default page
    buildWindowPage();
    stack_->addWidget(windowPage_);
    // Initialize with defaults until a canvas is attached
    winWidthSpin_->setValue(editor_defaults::kDefaultCanvasWidth);
    winHeightSpin_->setValue(editor_defaults::kDefaultCanvasHeight);
    stack_->setCurrentWidget(windowPage_);
}

// The heading for the current selection: the widget's friendly name over the
// selector that addresses it. Nothing selected means the window's own
// properties, which is what the panel falls back to.
void PropertiesPanel::showHeading(SelectionFrame* frame)
{
    if (frame == nullptr)
    {
        heading_->setText("Window");
        subheading_->setText("No widget selected");
        return;
    }

    QString friendly = QString::fromStdString(std::string(reflection::enum_to_string(frame->type())));
#define FRIENDLY_NAME_CASE(enum_name, widget_class)                  \
    if (frame->type() == widget_class::kWidgetType)                  \
    {                                                                \
        friendly = QString::fromUtf8(widget_class::kFriendlyName.data(), \
                                     static_cast<int>(widget_class::kFriendlyName.size())); \
    }
    DASHBOARD_WIDGET_TABLE(FRIENDLY_NAME_CASE)
#undef FRIENDLY_NAME_CASE

    heading_->setText(friendly);
    subheading_->setText(frame->objectName());
}

void PropertiesPanel::setCanvas(Canvas* canvas)
{
    canvas_ = canvas;
    if (canvas_ && windowPage_)
    {
        syncFromCanvas();
    }
}

namespace
{
    // Stops one editor from setting the width of the whole panel.
    //
    // Qt sizes a spin box's minimum to fit its widest possible value, and the
    // integer editors are ranged to INT_MAX -- so every one of them demanded
    // room for "2147483647". A combo box does the same for its longest item.
    // Between them the form's minimum came out wider than the panel, which is
    // what put a horizontal scrollbar under a form whose rows all fit. These are
    // free to grow into whatever width is available; they just may not insist
    // on it.
    constexpr int kMinEditorWidth = 60;

    void constrainEditorWidth(QWidget* editor)
    {
        editor->setMinimumWidth(kMinEditorWidth);
        if (auto* combo = qobject_cast<QComboBox*>(editor))
        {
            combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            combo->setMinimumContentsLength(6);
        }
    }

    template <typename T>
    // No fieldName/typeName parameters: they existed only to name the field in
    // the "unsupported type" warning, and that case is a static_assert now.
    QWidget* createLeafEditor(QWidget* parent, const T& value, const QString& path)
    {
        using FieldType = std::decay_t<T>;

        if constexpr (std::is_same_v<FieldType, std::string>)
        {
            auto* line = new QLineEdit(parent);
            line->setMinimumHeight(24);
            line->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            line->setText(QString::fromUtf8(value.data(), static_cast<int>(value.size())));
            line->setObjectName(QString("field:%1").arg(path));
            constrainEditorWidth(line);
            return line;
        }
        else if constexpr (std::is_same_v<FieldType, helpers::Color>)
        {
            // Create a widget with text field and color picker button
            auto* container = new QWidget(parent);
            auto* layout = new QHBoxLayout(container);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(4);
            
            auto* line = new QLineEdit(container);
            line->setMinimumHeight(24);
            line->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            line->setText(QString::fromStdString(value.value()));
            line->setObjectName(QString("field:%1").arg(path));
            layout->addWidget(line);
            
            // Color preview/picker button
            auto* colorBtn = new QPushButton(container);
            colorBtn->setMinimumHeight(24);
            colorBtn->setMaximumWidth(48);
            colorBtn->setText("🎨");
            colorBtn->setToolTip("Choose color");
            
            // Set button background to current color
            QColor currentColor(QString::fromStdString(value.value()));
            if (currentColor.isValid())
            {
                colorBtn->setStyleSheet(QString("QPushButton { background-color: %1; }").arg(currentColor.name()));
            }
            
            // Connect color picker
            QObject::connect(colorBtn, &QPushButton::clicked, container, [line, colorBtn]()
            {
                QColor current(line->text());
                QColor picked = QColorDialog::getColor(current, colorBtn->parentWidget(), "Choose Color");
                if (picked.isValid())
                {
                    line->setText(picked.name());
                    colorBtn->setStyleSheet(QString("QPushButton { background-color: %1; }").arg(picked.name()));
                }
            });
            
            // Update button color when text changes
            QObject::connect(line, &QLineEdit::textChanged, colorBtn, [colorBtn](const QString& text)
            {
                QColor color(text);
                if (color.isValid())
                {
                    colorBtn->setStyleSheet(QString("QPushButton { background-color: %1; }").arg(color.name()));
                }
            });
            
            layout->addWidget(colorBtn);
            container->setLayout(layout);
            container->setObjectName(QString("field:%1").arg(path));
            constrainEditorWidth(container);
            return container;
        }
        else if constexpr (std::is_enum_v<FieldType>)
        {
            auto* combo = new QComboBox(parent);
            for (const auto& enumVal : reflection::enum_traits<FieldType>::values())
            {
                const std::string_view value_str = reflection::enum_traits<FieldType>::to_string(enumVal);
                combo->addItem(QString::fromUtf8(value_str.data(), static_cast<int>(value_str.size())));
            }
            combo->setMinimumHeight(24);
            combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            // set current
            const std::string_view cur = reflection::enum_traits<FieldType>::to_string(value);
            const int idx = combo->findText(QString::fromUtf8(cur.data(), static_cast<int>(cur.size())));
            if (idx >= 0) combo->setCurrentIndex(idx);
            combo->setObjectName(QString("field:%1").arg(path));
            constrainEditorWidth(combo);
            return combo;
        }
        else if constexpr (std::is_same_v<FieldType, bool>)
        {
            auto* check = new QCheckBox(parent);
            check->setMinimumHeight(22);
            check->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
            check->setChecked(value);
            check->setObjectName(QString("field:%1").arg(path));
            return check;
        }
        else if constexpr (std::is_integral_v<FieldType>)
        {
            auto* spin = new QSpinBox(parent);
            spin->setMinimumHeight(24);
            spin->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

            // QSpinBox supports int only; clamp ranges to int domain to avoid overflow on unsigned configs
            const int minVal = std::is_signed_v<FieldType> ? std::numeric_limits<int>::min() : 0;
            const int maxVal = std::numeric_limits<int>::max();
            spin->setRange(minVal, maxVal);
            spin->setValue(static_cast<int>(value));
            spin->setObjectName(QString("field:%1").arg(path));
            constrainEditorWidth(spin);
            return spin;
        }
        else if constexpr (std::is_floating_point_v<FieldType>)
        {
            auto* dspin = new QDoubleSpinBox(parent);
            dspin->setDecimals(3);
            dspin->setMinimumHeight(24);
            dspin->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
            // Remove implicit 0..99.99 default range; use a very permissive range
            dspin->setRange(std::numeric_limits<double>::lowest(), std::numeric_limits<double>::max());
            dspin->setValue(static_cast<double>(value));
            dspin->setObjectName(QString("field:%1").arg(path));
            constrainEditorWidth(dspin);
            return dspin;
        }
        else
        {
            // A compile error, not a runtime warning. This used to build a
            // read-only "(unsupported type)" box, which readIntoConfig then had
            // no branch to read back -- so the field was reset to its default on
            // every Apply. A config field the panel cannot render is a silent
            // data-loss bug for whoever adds it, and they will not see the log
            // line. config_json.h has taken this position all along; the editor
            // is the half that was lenient.
            //
            // If you land here: add a branch above for the type, and a matching
            // one in readLeafFromWidget.
            static_assert(sizeof(T) == 0,
                          "properties_panel cannot build an editor for this config field type. "
                          "Add a branch to createLeafEditor and readLeafFromWidget.");
        }
    }

    // The label for one field of `Struct`: its friendly name, plus an info icon
    // carrying the description when the config supplies one.
    //
    // Shared by the top-level form and by nested structs. The nested case used
    // to print the raw field name -- `red_start_fraction` rather than "Red
    // Start" -- and had no way to show a description at all, because the label
    // was built inline from the string reflection handed it. Everything a field
    // is called now comes from one place.
    template <typename Struct>
    QWidget* createFieldLabel(QWidget* parent, std::string_view fieldName)
    {
        const std::string_view friendly = reflection::get_friendly_name<Struct>(fieldName);
        const QString text = QString::fromUtf8(friendly.data(), static_cast<int>(friendly.size()));

        const std::string_view description = reflection::get_description<Struct>(fieldName);
        if (description.empty())
        {
            auto* label = new QLabel(text, parent);
            label->setStyleSheet("QLabel { font-weight: 600; }");
            return label;
        }

        auto* container = new QWidget(parent);
        auto* layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        auto* textLabel = new QLabel(text, container);
        textLabel->setStyleSheet("QLabel { font-weight: 600; }");
        layout->addWidget(textLabel);

        auto* infoIcon = new QLabel("ⓘ", container);
        infoIcon->setStyleSheet("QLabel { color: #0066cc; font-size: 12px; }");
        infoIcon->setToolTip(
            QString::fromUtf8(description.data(), static_cast<int>(description.size())));
        layout->addWidget(infoIcon);

        layout->addStretch();
        container->setLayout(layout);
        return container;
    }

    // Label above the editor rather than beside it.
    //
    // Beside it, the label column took whatever width the longest name wanted
    // and the editors got the remainder -- which in a 259px panel was about
    // 90px, so "vehicle/speed_mps" displayed as "vehicle/sp" and the form grew a
    // horizontal scrollbar. Wrapping gives every editor the full width of the
    // panel, at the cost of a taller form. The values are the part you need to
    // be able to read.
    void applyFormStyle(QFormLayout* form)
    {
        form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setFormAlignment(Qt::AlignTop);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        form->setHorizontalSpacing(8);
        form->setVerticalSpacing(4);
    }

    template <typename T>
    QWidget* createEditorFor(QWidget* parent, std::string_view fieldName, T& ref, std::string_view typeName, const QString& path)
    {
        using FieldType = std::decay_t<T>;
        if constexpr (reflection::is_std_vector<FieldType>::value)
        {
            using Elem = typename reflection::is_std_vector<FieldType>::value_type;
            // Container for vector elements with add/remove controls
            auto* container = new QWidget(parent);
            auto* outer = new QVBoxLayout(container);
            outer->setContentsMargins(0,0,0,0);
            outer->setSpacing(6);

            // Header with (+) and (-)
            auto* header = new QWidget(container);
            auto* headerLayout = new QHBoxLayout(header);
            headerLayout->setContentsMargins(0,0,0,0);
            headerLayout->setSpacing(6);
            auto* addBtn = new QPushButton("Add", header);
            addBtn->setToolTip("Add an entry to the end of the list");
            headerLayout->addStretch();
            headerLayout->addWidget(addBtn);
            header->setLayout(headerLayout);
            outer->addWidget(header);

            // Items area
            auto* items = new QWidget(container);
            auto* itemsLayout = new QVBoxLayout(items);
            itemsLayout->setContentsMargins(4,4,4,4);
            itemsLayout->setSpacing(8);

            auto addRow = [items, itemsLayout, fieldName, typeName, path](const Elem* initValue)
            {
                const int idx = itemsLayout->count();
                auto* row = new QWidget(items);
                auto* h = new QHBoxLayout(row);
                h->setContentsMargins(0,2,0,2);
                h->setSpacing(6);
                Elem valueToUse = initValue ? *initValue : Elem{};
                const QString childPath = QString("%1[%2]").arg(path).arg(idx);

                // createEditorFor, not createLeafEditor: an element that is
                // itself a reflected struct needs the nested-struct branch. No
                // config has a vector of structs today, which is exactly why the
                // next one would have hit the leaf path and been silently reset
                // to defaults on Apply.
                //
                // The editor is item 0 of the row and the remove button item 1;
                // readEditorInto() reads the row positionally, so anything added
                // here has to keep the editor first.
                auto* childEditor = createEditorFor<Elem>(row, fieldName, valueToUse, typeName, childPath);
                h->addWidget(childEditor, 1);

                // Remove this row, rather than "remove the last one". The single
                // global button could only drop entries off the end, so taking
                // an entry out of the middle of a list meant retyping every
                // value after it.
                auto* rowRemove = new QPushButton("✕", row);
                rowRemove->setToolTip("Remove this entry");
                rowRemove->setFixedWidth(24);
                rowRemove->setFlat(true);
                QObject::connect(rowRemove, &QPushButton::clicked, row, [row]
                {
                    // Detached from the layout AND hidden, because the read walks
                    // the layout and deleteLater does not take it out until the
                    // event loop next runs -- so an Apply in between would see a
                    // row the user had already removed.
                    row->hide();
                    row->setParent(nullptr);
                    row->deleteLater();
                });
                h->addWidget(rowRemove, 0);

                row->setLayout(h);
                itemsLayout->addWidget(row);
            };

            // Populate existing elements
            for (auto& elem : ref)
            {
                addRow(&elem);
            }

            QObject::connect(addBtn, &QPushButton::clicked, container, [addRow]{ addRow(nullptr); });

            items->setLayout(itemsLayout);
            outer->addWidget(items);
            container->setLayout(outer);
            return container;
        }
        else if constexpr (reflection::is_reflected_struct<FieldType>::value)
        {
            // Build an inset group with the nested struct's fields
            auto* frame = new QFrame(parent);
            frame->setObjectName("insetStructFrame");
            frame->setFrameShape(QFrame::StyledPanel);
            frame->setFrameShadow(QFrame::Raised);
            frame->setStyleSheet("#insetStructFrame{ border:1px solid palette(mid); border-radius:4px; }");

            auto* form = new QFormLayout(frame);
            applyFormStyle(form);
            form->setContentsMargins(8,8,8,8);

            reflection::visit_fields(ref, [&](std::string_view childName, auto& childRef, std::string_view childType)
            {
                // The path keeps the raw field name: it only names the editor
                // for addressing, and a friendly name would change whenever
                // someone reworded a label.
                const QString childPath =
                    QString("%1.%2").arg(path,
                                         QString::fromUtf8(childName.data(),
                                                           static_cast<int>(childName.size())));
                QWidget* childEditor = createEditorFor(frame, childName, childRef, childType, childPath);
                form->addRow(createFieldLabel<FieldType>(frame, childName), childEditor);
            });

            frame->setLayout(form);
            return frame;
        }
        else
        {
            return createLeafEditor<FieldType>(parent, ref, path);
        }
    }

    // ------------------------------------------------------------- reading back
    //
    // The read walks the widget subtree that the build produced, and is handed
    // each editor directly.
    //
    // It used to be two independent walks joined only by a convention: the build
    // stamped "field:<path>" onto every editor and the read went looking for that
    // string with findChild. The two had to agree on traversal order AND on how a
    // path is spelled, with nothing to enforce either -- a field that fell out of
    // step stopped round-tripping in silence, and the fix was always to go and
    // read both walks. findChild made it worse: it searches the whole subtree, so
    // a page that had not been destroyed yet could answer for the one being read,
    // which is the bug discardCurrentPage() below still carries a comment about.
    //
    // Structure is now the only thing the two sides share, and they cannot
    // disagree about it, because one of them built it. The objectNames are still
    // set, but purely so a test or the agent interface can address a field by
    // name; nothing here reads them.

    // The field widget of row `row` of a form.
    QWidget* formFieldAt(QFormLayout* form, int row)
    {
        if (form == nullptr || row < 0 || row >= form->rowCount())
        {
            return nullptr;
        }
        QLayoutItem* item = form->itemAt(row, QFormLayout::FieldRole);
        return item ? item->widget() : nullptr;
    }

    template <typename T>
    void readEditorInto(QWidget* editor, T& out);

    // A leaf: the widget handed in IS the editor the build made for this type.
    template <typename T>
    void readLeafInto(QWidget* editor, T& out)
    {
        using FieldType = std::decay_t<T>;
        if (editor == nullptr)
        {
            return;
        }

        if constexpr (std::is_same_v<FieldType, std::string>)
        {
            if (auto* w = qobject_cast<QLineEdit*>(editor)) out = w->text().toStdString();
        }
        else if constexpr (std::is_same_v<FieldType, helpers::Color>)
        {
            // A container: the line edit, then the picker button.
            if (auto* layout = editor->layout(); layout != nullptr && layout->count() > 0)
            {
                if (auto* w = qobject_cast<QLineEdit*>(layout->itemAt(0)->widget()))
                {
                    out = helpers::Color(w->text().toStdString());
                }
            }
        }
        else if constexpr (std::is_same_v<FieldType, bool>)
        {
            if (auto* w = qobject_cast<QCheckBox*>(editor)) out = w->isChecked();
        }
        else if constexpr (std::is_enum_v<FieldType>)
        {
            if (auto* w = qobject_cast<QComboBox*>(editor))
            {
                // Non-throwing: this runs on a user-interaction path, and an
                // exception here would unwind through QApplication::notify().
                // The combo is populated from the enum, so a miss means the page
                // is stale -- keep the caller's existing value rather than
                // inventing one.
                if (const auto v = reflection::enum_traits<FieldType>::try_from_string(
                        w->currentText().toStdString()))
                {
                    out = *v;
                }
                else
                {
                    SPDLOG_WARN("Ignoring unknown value '{}' for field '{}'.",
                                w->currentText().toStdString(),
                                editor->objectName().toStdString());
                }
            }
        }
        else if constexpr (std::is_integral_v<FieldType>)
        {
            if (auto* w = qobject_cast<QSpinBox*>(editor)) out = static_cast<FieldType>(w->value());
        }
        else if constexpr (std::is_floating_point_v<FieldType>)
        {
            if (auto* w = qobject_cast<QDoubleSpinBox*>(editor))
                out = static_cast<FieldType>(w->value());
        }
    }

    // Reads whatever createEditorFor() built for a field of type T.
    template <typename T>
    void readEditorInto(QWidget* editor, T& out)
    {
        using FieldType = std::decay_t<T>;
        if (editor == nullptr)
        {
            return;
        }

        if constexpr (reflection::is_std_vector<FieldType>::value)
        {
            using Elem = typename reflection::is_std_vector<FieldType>::value_type;

            // The container's second entry is the items area; its rows are the
            // elements. Read the rows that are there NOW rather than the ones
            // that were there at build time -- Add and Remove change the count
            // after the fact, which is exactly what a positional index into a
            // list captured during the build could not have survived.
            auto* outer = editor->layout();
            if (outer == nullptr || outer->count() < 2)
            {
                return;
            }
            QWidget* items = outer->itemAt(1)->widget();
            QLayout* itemsLayout = items ? items->layout() : nullptr;
            if (itemsLayout == nullptr)
            {
                return;
            }

            FieldType result;
            for (int i = 0; i < itemsLayout->count(); ++i)
            {
                QWidget* row = itemsLayout->itemAt(i)->widget();
                QLayout* rowLayout = row ? row->layout() : nullptr;
                if (rowLayout == nullptr || rowLayout->count() < 1)
                {
                    continue;
                }
                // [0] is the element's editor, [1] its remove button. Keep this
                // in step with addRow(): the row is read by position, so putting
                // anything before the editor silently reads the wrong widget.
                // Seeded from the element already at that index so a field the
                // form does not render survives, as in the struct case below.
                Elem value = (static_cast<std::size_t>(result.size()) < out.size())
                                 ? out[static_cast<std::size_t>(result.size())]
                                 : Elem{};
                readEditorInto<Elem>(rowLayout->itemAt(0)->widget(), value);
                result.push_back(std::move(value));
            }
            out = std::move(result);
        }
        else if constexpr (reflection::is_reflected_struct<FieldType>::value)
        {
            // An inset QFrame carrying a QFormLayout, one row per field, built in
            // reflection order -- so read them back in reflection order.
            auto* form = qobject_cast<QFormLayout*>(editor->layout());
            int row = 0;
            reflection::visit_fields(out, [&](std::string_view /*name*/, auto& ref,
                                              std::string_view /*type*/)
            {
                readEditorInto(formFieldAt(form, row), ref);
                ++row;
            });
        }
        else
        {
            readLeafInto<FieldType>(editor, out);
        }
    }

    // Patches `cfg` in place from the form's editors and returns it.
    //
    // The seed matters. This used to start from a default-constructed Config, so
    // every field the form could not render -- an unsupported type, a missing
    // editor -- was written back as the type default, silently destroying it on
    // each Apply. Starting from the widget's live config means a field the form
    // does not touch simply survives, which is what the agent's set_config path
    // has always done (see patchedConfig in dashboard/agent/widget_methods.cpp).
    template <typename Config>
    Config readIntoConfig(QFormLayout* form, Config cfg)
    {
        int row = 0;
        reflection::visit_fields(cfg, [&](std::string_view /*name*/, auto& ref,
                                          std::string_view /*typeName*/)
        {
            readEditorInto(formFieldAt(form, row), ref);
            ++row;
        });
        return cfg;
    }

    template <typename Config>
    QWidget* buildFormFromConfig(QWidget* parent, const Config& cfg)
    {
        auto* page = new QWidget(parent);
        auto* vbox = new QVBoxLayout(page);
        vbox->setContentsMargins(0,0,0,0);
        vbox->setSpacing(0);
        auto* scroll = new QScrollArea(page);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto* scrollContent = new QWidget(scroll);
        auto* form = new QFormLayout();
        applyFormStyle(form);
        form->setContentsMargins(10, 8, 10, 8);
        form->setVerticalSpacing(10);
        reflection::visit_fields<Config>(cfg, [&](std::string_view name, auto& ref, std::string_view type)
        {
            const QString fieldPath = QString::fromUtf8(name.data(), static_cast<int>(name.size()));
            QWidget* editor = createEditorFor(scrollContent, name, ref, type, fieldPath);
            form->addRow(createFieldLabel<Config>(scrollContent, name), editor);
        });
        scrollContent->setLayout(form);
        scroll->setWidget(scrollContent);
        vbox->addWidget(scroll, 1); // let the scroll area take all remaining space
        auto* applyBtn = new QPushButton("Apply", page);
        applyBtn->setMinimumHeight(28);
        applyBtn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto* bottom = new QHBoxLayout();
        bottom->setContentsMargins(8,8,8,8);
        bottom->addWidget(applyBtn);
        bottom->setSizeConstraint(QLayout::SetMinimumSize);
        vbox->addLayout(bottom, 0); // persistent bottom bar
        page->setLayout(vbox);

        PropertiesPanel* that = qobject_cast<PropertiesPanel*>(parent);
        QObject::connect(applyBtn, &QPushButton::clicked, page, [form, that]()
        {
            if (!that || !that->selected()) return;
            QWidget* w = that->selected();
            SelectionFrame* frame = qobject_cast<SelectionFrame*>(w);
            if (!frame) return; // editor always wraps in SelectionFrame

            bool applied = false;

            // One history entry per Apply, closed when this handler returns. The
            // transaction discards itself if every field came back the same, so
            // pressing Apply without changing anything does not add an undo step
            // that appears to do nothing.
            const Canvas::EditTransaction tx(that->canvas(), Canvas::EditSource::Widget);

            // Visit the stored config rather than switching on the type and
            // casting the live widget back to it. The variant already knows
            // which alternative it holds, so there is no cast to get wrong --
            // the previous version needed qobject_cast specifically because a
            // static_cast would have been undefined behaviour if the frame's
            // declared type and its actual child ever disagreed.
            //
            // It also reads the seed from the frame's own config, which is the
            // configured value; the live widget's is the clamped one, so seeding
            // from there would have written the clamp back on every Apply.
            std::visit(
                [&](const auto& current)
                {
                    using cfg_t = std::decay_t<decltype(current)>;
                    if constexpr (!std::is_same_v<cfg_t, std::monostate>)
                    {
                        applied = frame->applyConfig(readIntoConfig<cfg_t>(form, current));
                    }
                },
                frame->config());

            if (!applied)
            {
                SPDLOG_ERROR("Apply did nothing for '{}': the frame holds no configuration.",
                             frame->objectName().toStdString());
            }
        });
        return page;
    }
}

void PropertiesPanel::buildWindowPage()
{
    if (windowPage_) return;
    windowPage_ = new QWidget(this);
    auto* form = new QFormLayout(windowPage_);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setFormAlignment(Qt::AlignTop);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setContentsMargins(8,8,8,8);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(10);


    // Named on the same rule as the per-widget form editors ("field:<path>"),
    // so the window's properties are addressable by name rather than by their
    // position in the layout -- which is what a test or the agent interface
    // would otherwise have to depend on.
    winNameEdit_ = new QLineEdit(windowPage_);
    winNameEdit_->setObjectName("window:name");

    winWidthSpin_ = new QSpinBox(windowPage_);
    winWidthSpin_->setRange(100u, 10000u);
    winWidthSpin_->setObjectName("window:width");

    winHeightSpin_ = new QSpinBox(windowPage_);
    winHeightSpin_->setRange(100u, 10000u);
    winHeightSpin_->setObjectName("window:height");

    winBgColorEdit_ = new QLineEdit(windowPage_);
    winBgColorEdit_->setPlaceholderText("#RRGGBB");
    winBgColorEdit_->setObjectName("window:background_color");

    form->addRow("Name", winNameEdit_);
    form->addRow("Width", winWidthSpin_);
    form->addRow("Height", winHeightSpin_);
    form->addRow("Background Color", winBgColorEdit_);
    windowPage_->setLayout(form);

    connect(winNameEdit_, &QLineEdit::textEdited, this, [this]{ applyWindowEdits(); });
    connect(winWidthSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int){ applyWindowEdits(); });
    connect(winHeightSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int){ applyWindowEdits(); });
    connect(winBgColorEdit_, &QLineEdit::textEdited, this, [this]{ applyWindowEdits(); });

    // Close the history entry when the field is done, not on every keystroke.
    //
    // applyWindowEdits() opens one on the first change and beginEdit() collapses
    // the rest, so the pair is "one entry per edited field" rather than one per
    // character. editingFinished covers both Enter and focus loss, which is what
    // makes clicking away from the field commit it.
    for (QLineEdit* edit : {winNameEdit_, winBgColorEdit_})
    {
        connect(edit, &QLineEdit::editingFinished, this, [this]{ commitWindowEdits(); });
    }
    for (QSpinBox* spin : {winWidthSpin_, winHeightSpin_})
    {
        connect(spin, &QAbstractSpinBox::editingFinished, this, [this]{ commitWindowEdits(); });
    }
}

void PropertiesPanel::discardCurrentPage()
{
    if (currentPage_)
    {
        // Take it out of the stack before deleting so the stack never holds a
        // dangling entry. This used to leak a page into the stack on every
        // selection.
        stack_->removeWidget(currentPage_);

        // Destroyed now, not deleteLater'd. removeWidget() only takes the page
        // out of the layout -- it stays a child of the stack until it is really
        // deleted, and every field editor on it keeps its "field:<path>"
        // objectName. Anything doing findChild() before the event loop next runs
        // would then find the *old* page's editor and read the previously
        // selected widget's values. MainWindow::rebuildWidget destroys
        // synchronously for the same reason.
        //
        // Safe here because selection changes arrive from the canvas, never from
        // inside one of this page's own widgets.
        delete currentPage_;
        currentPage_ = nullptr;
    }
}

void PropertiesPanel::showPage(QWidget* page)
{
    currentPage_ = page;
    stack_->addWidget(page);
    stack_->setCurrentWidget(page);
}

void PropertiesPanel::showUnsupported(const QString& name)
{
    auto* page = new QWidget(this);
    auto* v = new QVBoxLayout(page);
    v->addWidget(new QLabel(QString("%1 properties not yet supported").arg(name)));
    v->addStretch();
    showPage(page);
}

void PropertiesPanel::setSelectedWidget(QWidget* w)
{
    selected_ = w;

    // Always start from a clean page. The form's values come from this
    // selection's live config, so nothing from the previous one may survive.
    discardCurrentPage();
    showHeading(qobject_cast<SelectionFrame*>(w));

    if (!w)
    {
        // Show window properties when no widget selected
        if (windowPage_) stack_->setCurrentWidget(windowPage_);
        if (canvas_)
        {
            syncFromCanvas();
        }
        return;
    }

    // Unwrap SelectionFrame for UI classification
    QWidget* uiWidget = w;
    if (auto* frame = qobject_cast<SelectionFrame*>(uiWidget)) uiWidget = frame->child();

    // No child means the frame holds nothing to configure. Bail rather than
    // fall through to `uiWidget = w`, which fed a SelectionFrame* into a
    // static_cast to an unrelated widget type below.
    if (!uiWidget)
    {
        showUnsupported(w->metaObject()->className());
        return;
    }

    // Build the form from the frame's stored config, not from the live widget's.
    // They differ: the widget holds the clamped copy widget_factory built it
    // from, so a field the clamp moved would be shown as the clamped value and
    // then written back on Apply, quietly editing the user's config for them.
    QWidget* page = nullptr;
    if (auto* frame = qobject_cast<SelectionFrame*>(w))
    {
        std::visit(
            [&](const auto& cfg)
            {
                using cfg_t = std::decay_t<decltype(cfg)>;
                if constexpr (!std::is_same_v<cfg_t, std::monostate>)
                {
                    page = buildFormFromConfig<cfg_t>(this, cfg);
                }
            },
            frame->config());
    }

    if (page)
    {
        showPage(page);
        return;
    }

    // Other types unsupported for now
    showUnsupported(w->metaObject()->className());
    // leave as unsupported page
}

// Removed per reflect-first UI goal; wiring updates will follow later.

void PropertiesPanel::applyWindowEdits()
{
    if (isSyncing_) return; // avoid pushing during UI sync
    if (!canvas_) return;

    // These are document edits like any other, and they were the one mutation
    // path that never said so. The window's name, size and background changed
    // the canvas without opening a history entry, so they could not be undone --
    // and worse, the *next* edit's beginEdit() snapshotted the already-changed
    // state, baking them in permanently. The dirty flag noticed (isDirty
    // compares snapshots) but nothing emitted historyChanged, so the title bar
    // kept its old text until some unrelated edit refreshed it.
    //
    // beginEdit() collapses repeated calls, so this is the *first* keystroke
    // capturing the pre-edit state; commitWindowEdits() closes it. Tagged
    // Window so that an edit from anywhere else closes it rather than merging
    // with it -- editingFinished needs a focus change, and the agent interface
    // never moves focus.
    canvas_->beginEdit(Canvas::EditSource::Window);

    // The name is round-tripped through the canvas into the saved YAML. It used
    // to be collected here and never read, while save hardcoded its own.
    canvas_->setWindowName(winNameEdit_->text().toStdString());

    // Only apply a colour that is actually a colour. Half-typed text arrives
    // here on every keystroke -- "#ff00" on the way to "#ff0000" -- and an
    // unparseable value silently becomes the fallback, so without this the
    // preview flickered through black as you typed. Same check the loader
    // applies (see validate_app_config), so the editor and the file agree on
    // what a colour is.
    const QString bg = winBgColorEdit_->text();
    if (!bg.isEmpty() && helpers::Color::isValidFormat(bg.toStdString()))
    {
        canvas_->setBackgroundColor(bg);
    }
    if (winWidthSpin_->value() > 0 && winHeightSpin_->value() > 0) {
        // Resize the central canvas viewport for preview purposes
        canvas_->resize(winWidthSpin_->value(), winHeightSpin_->value());
    }
}

void PropertiesPanel::commitWindowEdits()
{
    if (isSyncing_ || !canvas_)
    {
        return;
    }
    // No-op when nothing is open, and commitEdit() itself discards an entry
    // whose before and after match -- so tabbing through the fields without
    // changing anything adds nothing to the history.
    canvas_->commitEdit();
}

void PropertiesPanel::syncFromCanvas()
{
    if (!canvas_ || !windowPage_) return;
    const QSignalBlocker b1(winNameEdit_);
    const QSignalBlocker b2(winWidthSpin_);
    const QSignalBlocker b3(winHeightSpin_);
    const QSignalBlocker b4(winBgColorEdit_);
    isSyncing_ = true;
    winNameEdit_->setText(QString::fromStdString(canvas_->windowName()));
    winWidthSpin_->setValue(canvas_->width());
    winHeightSpin_->setValue(canvas_->height());
    winBgColorEdit_->setText(canvas_->getBackgroundColorHex());
    isSyncing_ = false;
}

#include "editor/moc_properties_panel.cpp"

