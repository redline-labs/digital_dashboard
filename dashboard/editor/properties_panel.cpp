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
  widgetPages_(),
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
    layout->setSpacing(8);
    layout->addWidget(new QLabel("Properties"));
    layout->addWidget(stack_);
    layout->addStretch();
    setLayout(layout);

    // Default page
    buildWindowPage();
    stack_->addWidget(windowPage_);
    // Initialize with defaults until a canvas is attached
    winWidthSpin_->setValue(editor_defaults::kDefaultCanvasWidth);
    winHeightSpin_->setValue(editor_defaults::kDefaultCanvasHeight);
    stack_->setCurrentWidget(windowPage_);
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
    template <typename T>
    QWidget* createLeafEditor(QWidget* parent, std::string_view fieldName, const T& value, std::string_view typeName, const QString& path)
    {
        using FieldType = std::decay_t<T>;

        if constexpr (std::is_same_v<FieldType, std::string>)
        {
            auto* line = new QLineEdit(parent);
            line->setMinimumHeight(24);
            line->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            line->setText(QString::fromUtf8(value.data(), static_cast<int>(value.size())));
            line->setObjectName(QString("field:%1").arg(path));
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
            return dspin;
        }
        else
        {
            auto* line = new QLineEdit(parent);
            line->setReadOnly(true);
            line->setPlaceholderText("(unsupported type)");
            SPDLOG_WARN("Unsupported type: '{}' for field '{}'", typeName, fieldName);
            line->setObjectName(QString("field:%1").arg(path));
            return line;
        }
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
            auto* removeBtn = new QPushButton("Remove", header);
            headerLayout->addStretch();
            headerLayout->addWidget(addBtn);
            headerLayout->addWidget(removeBtn);
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
                auto* idxLabel = new QLabel(QString("[%1]").arg(idx), row);
                h->addWidget(idxLabel);
                Elem valueToUse = initValue ? *initValue : Elem{};
                const QString childPath = QString("%1[%2]").arg(path).arg(idx);
                auto* childEditor = createLeafEditor<Elem>(row, fieldName, valueToUse, typeName, childPath);
                h->addWidget(childEditor, 1);
                row->setLayout(h);
                itemsLayout->addWidget(row);
            };

            // Populate existing elements
            for (auto& elem : ref)
            {
                addRow(&elem);
            }

            // Wire buttons
            QObject::connect(addBtn, &QPushButton::clicked, container, [addRow]{ addRow(nullptr); });
            QObject::connect(removeBtn, &QPushButton::clicked, container, [itemsLayout]()
            {
                const int count = itemsLayout->count();
                if (count <= 0) return;
                auto* item = itemsLayout->takeAt(count - 1);
                if (item)
                {
                    if (auto* w = item->widget()) { w->deleteLater(); }
                    delete item;
                }
            });

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
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            form->setFormAlignment(Qt::AlignTop);
            form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            form->setContentsMargins(8,8,8,8);
            form->setHorizontalSpacing(10);
            form->setVerticalSpacing(8);

            reflection::visit_fields(ref, [&](std::string_view childName, auto& childRef, std::string_view childType)
            {
                const QString childLabel = QString::fromUtf8(childName.data(), static_cast<int>(childName.size()));
                const QString childPath = QString("%1.%2").arg(path, childLabel);
                QWidget* childEditor = createEditorFor(frame, childName, childRef, childType, childPath);
                form->addRow(childLabel, childEditor);
            });

            frame->setLayout(form);
            return frame;
        }
        else
        {
            return createLeafEditor<FieldType>(parent, fieldName, ref, typeName, path);
        }
    }

    // Reading helpers
    template <typename T>
    void readLeafFromWidget(QWidget* page, const QString& path, T& out)
    {
        using FieldType = std::decay_t<T>;
        const QString on = QString("field:%1").arg(path);
        if constexpr (std::is_same_v<FieldType, std::string>)
        {
            if (auto* w = page->findChild<QLineEdit*>(on)) out = w->text().toStdString();
        }
        else if constexpr (std::is_same_v<FieldType, helpers::Color>)
        {
            if (auto* w = page->findChild<QLineEdit*>(on)) out = helpers::Color(w->text().toStdString());
        }
        else if constexpr (std::is_same_v<FieldType, bool>)
        {
            if (auto* w = page->findChild<QCheckBox*>(on)) out = w->isChecked();
        }
        else if constexpr (std::is_enum_v<FieldType>)
        {
            if (auto* w = page->findChild<QComboBox*>(on))
            {
                out = reflection::enum_traits<FieldType>::from_string(w->currentText().toStdString());
            }
        }
        else if constexpr (std::is_integral_v<FieldType>)
        {
            if (auto* w = page->findChild<QSpinBox*>(on)) out = static_cast<FieldType>(w->value());
        }
        else if constexpr (std::is_floating_point_v<FieldType>)
        {
            if (auto* w = page->findChild<QDoubleSpinBox*>(on)) out = static_cast<FieldType>(w->value());
        }
    }

    template <typename Config>
    Config readIntoConfig(QWidget* page, const QString& basePath = "")
    {
        Config cfg{};

        reflection::visit_fields(cfg, [&](std::string_view name, auto& ref, std::string_view /*typeName*/)
        {
            const QString field = QString::fromUtf8(name.data(), static_cast<int>(name.size()));
            const QString path = basePath.isEmpty() ? field : QString("%1.%2").arg(basePath, field);
            using FieldType = std::decay_t<decltype(ref)>;
            if constexpr (reflection::is_reflected_struct<FieldType>::value)
            {
                ref = readIntoConfig<FieldType>(page, path);
            }
            else if constexpr (reflection::is_std_vector<FieldType>::value)
            {
                using Elem = typename reflection::is_std_vector<FieldType>::value_type;
                FieldType outVec;
                for (int i = 0; ; ++i)
                {
                    const QString elemPath = QString("%1[%2]").arg(path).arg(i);
                    const QString on = QString("field:%1").arg(elemPath);
                    QWidget* any = page->findChild<QWidget*>(on);
                    if (!any) break;
                    Elem v{};
                    readLeafFromWidget(page, elemPath, v);
                    outVec.push_back(std::move(v));
                }
                ref = std::move(outVec);
            }
            else
            {
                readLeafFromWidget(page, path, ref);
            }
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
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setFormAlignment(Qt::AlignTop);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        form->setContentsMargins(8,8,8,8);
        form->setHorizontalSpacing(12);
        form->setVerticalSpacing(10);
        reflection::visit_fields<Config>(cfg, [&](std::string_view name, auto& ref, std::string_view type)
        {
            // Use friendly name from metadata if available, otherwise use field name
            std::string_view displayName = reflection::get_friendly_name<Config>(name);
            const QString labelText = QString::fromUtf8(displayName.data(), static_cast<int>(displayName.size()));
            const QString fieldPath = QString::fromUtf8(name.data(), static_cast<int>(name.size()));
            QWidget* editor = createEditorFor(scrollContent, name, ref, type, fieldPath);
            
            // Check if there's a description for this field
            std::string_view description = reflection::get_description<Config>(name);
            
            if (!description.empty())
            {
                // Create a label widget with info icon for fields with descriptions
                auto* labelWidget = new QWidget(scrollContent);
                auto* labelLayout = new QHBoxLayout(labelWidget);
                labelLayout->setContentsMargins(0, 0, 0, 0);
                labelLayout->setSpacing(4);
                
                auto* textLabel = new QLabel(labelText, labelWidget);
                labelLayout->addWidget(textLabel);
                
                // Add info icon with tooltip
                auto* infoIcon = new QLabel("ⓘ", labelWidget);
                infoIcon->setStyleSheet("QLabel { color: #0066cc; font-size: 12px; }");
                const QString tooltip = QString::fromUtf8(description.data(), static_cast<int>(description.size()));
                infoIcon->setToolTip(tooltip);
                labelLayout->addWidget(infoIcon);
                
                labelLayout->addStretch();
                labelWidget->setLayout(labelLayout);
                
                form->addRow(labelWidget, editor);
            }
            else
            {
                // No description, use simple text label
                form->addRow(labelText, editor);
            }
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
        QObject::connect(applyBtn, &QPushButton::clicked, page, [page, that]()
        {
            if (!that || !that->selected()) return;
            QWidget* w = that->selected();
            SelectionFrame* frame = qobject_cast<SelectionFrame*>(w);
            if (!frame) return; // editor always wraps in SelectionFrame

            const widget_type_t type = frame->type();
            switch (type)
            {
                case widget_type_t::static_text:
                    frame->applyConfig(
                        readIntoConfig<StaticTextWidget::config_t>(page)
                    );
                    break;

                case widget_type_t::background_rect:
                    frame->applyConfig(
                        readIntoConfig<BackgroundRectWidget::config_t>(page)
                    );
                    break;

                case widget_type_t::mercedes_190e_cluster_gauge:
                    frame->applyConfig(
                        readIntoConfig<Mercedes190EClusterGauge::config_t>(page)
                    );
                    break;

                case widget_type_t::mercedes_190e_speedometer:
                    frame->applyConfig(
                        readIntoConfig<Mercedes190ESpeedometer::config_t>(page)
                    );
                    break;

                case widget_type_t::mercedes_190e_tachometer:
                    frame->applyConfig(
                        readIntoConfig<Mercedes190ETachometer::config_t>(page)
                    );
                    break;

                case widget_type_t::motec_c125_tachometer:
                    frame->applyConfig(
                        readIntoConfig<MotecC125Tachometer::config_t>(page)
                    );
                    break;

                case widget_type_t::motec_cdl3_tachometer:
                    frame->applyConfig(
                        readIntoConfig<MotecCdl3Tachometer::config_t>(page)
                    );
                    break;

                case widget_type_t::sparkline:
                    frame->applyConfig(
                        readIntoConfig<SparklineItem::config_t>(page)
                    );
                    break;

                case widget_type_t::value_readout:
                    frame->applyConfig(
                        readIntoConfig<ValueReadoutWidget::config_t>(page)
                    );
                    break;

                case widget_type_t::carplay:
                    frame->applyConfig(
                        readIntoConfig<CarPlayWidget::config_t>(page)
                    );
                    break;

                case widget_type_t::mercedes_190e_telltale:
                    frame->applyConfig(
                        readIntoConfig<Mercedes190ETelltale::config_t>(page)
                    );
                    break;

                case widget_type_t::unknown:
                default:
                    SPDLOG_WARN("Unknown/unhandled widget type: '{}'", reflection::enum_to_string(type));
                    break;
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


    winNameEdit_ = new QLineEdit(windowPage_);
    
    winWidthSpin_ = new QSpinBox(windowPage_);
    winWidthSpin_->setRange(100u, 10000u);

    winHeightSpin_ = new QSpinBox(windowPage_);
    winHeightSpin_->setRange(100u, 10000u);

    winBgColorEdit_ = new QLineEdit(windowPage_);
    winBgColorEdit_->setPlaceholderText("#RRGGBB");

    form->addRow("Name", winNameEdit_);
    form->addRow("Width", winWidthSpin_);
    form->addRow("Height", winHeightSpin_);
    form->addRow("Background Color", winBgColorEdit_);
    windowPage_->setLayout(form);

    connect(winNameEdit_, &QLineEdit::textEdited, this, [this]{ applyWindowEdits(); });
    connect(winWidthSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int){ applyWindowEdits(); });
    connect(winHeightSpin_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int){ applyWindowEdits(); });
    connect(winBgColorEdit_, &QLineEdit::textEdited, this, [this]{ applyWindowEdits(); });
}

void PropertiesPanel::showUnsupported(const QString& name)
{
    auto* page = new QWidget(this);
    auto* v = new QVBoxLayout(page);
    v->addWidget(new QLabel(QString("%1 properties not yet supported").arg(name)));
    v->addStretch();
    stack_->addWidget(page);
    stack_->setCurrentWidget(page);
}

void PropertiesPanel::setSelectedWidget(QWidget* w)
{
    selected_ = w;
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
    if (!uiWidget) uiWidget = w; // fallback
    const QString qtClass = uiWidget->metaObject()->className();
    if (widgetPages_.contains(qtClass))
    {
        stack_->setCurrentWidget(widgetPages_.value(qtClass));
        return;
    }

    // Map widget instance to type; in editor we always have SelectionFrame
    const widget_type_t type = [w]()
    {
        if (auto* f = qobject_cast<SelectionFrame*>(w)) return f->type();
        return widget_type_t::unknown;
    }();
    QWidget* page = nullptr;
    switch (type)
    {
        case widget_type_t::static_text: page = buildFormFromConfig<StaticTextWidget::config_t>(this, static_cast<StaticTextWidget*>(uiWidget)->getConfig()); break;
        case widget_type_t::background_rect: page = buildFormFromConfig<BackgroundRectWidget::config_t>(this, static_cast<BackgroundRectWidget*>(uiWidget)->getConfig()); break;
        case widget_type_t::mercedes_190e_cluster_gauge: page = buildFormFromConfig<Mercedes190EClusterGauge::config_t>(this, static_cast<Mercedes190EClusterGauge*>(uiWidget)->getConfig()); break;
        case widget_type_t::mercedes_190e_speedometer: page = buildFormFromConfig<Mercedes190ESpeedometer::config_t>(this, static_cast<Mercedes190ESpeedometer*>(uiWidget)->getConfig()); break;
        case widget_type_t::mercedes_190e_tachometer: page = buildFormFromConfig<Mercedes190ETachometer::config_t>(this, static_cast<Mercedes190ETachometer*>(uiWidget)->getConfig()); break;
        case widget_type_t::motec_c125_tachometer: page = buildFormFromConfig<MotecC125Tachometer::config_t>(this, static_cast<MotecC125Tachometer*>(uiWidget)->getConfig()); break;
        case widget_type_t::motec_cdl3_tachometer: page = buildFormFromConfig<MotecCdl3Tachometer::config_t>(this, static_cast<MotecCdl3Tachometer*>(uiWidget)->getConfig()); break;
        case widget_type_t::sparkline: page = buildFormFromConfig<SparklineItem::config_t>(this, static_cast<SparklineItem*>(uiWidget)->getConfig()); break;
        case widget_type_t::value_readout: page = buildFormFromConfig<ValueReadoutWidget::config_t>(this, static_cast<ValueReadoutWidget*>(uiWidget)->getConfig()); break;
        case widget_type_t::carplay: page = buildFormFromConfig<CarPlayWidget::config_t>(this, static_cast<CarPlayWidget*>(uiWidget)->getConfig()); break;
        case widget_type_t::mercedes_190e_telltale: page = buildFormFromConfig<Mercedes190ETelltale::config_t>(this, static_cast<Mercedes190ETelltale*>(uiWidget)->getConfig()); break;
        case widget_type_t::unknown: default: break;
    }

    if (page)
    {
        widgetPages_.insert(qtClass, page);
        stack_->addWidget(page);
        stack_->setCurrentWidget(page);
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
    // Apply background immediately; name/size can be used for serialization later
    if (!winBgColorEdit_->text().isEmpty())
    {
        canvas_->setBackgroundColor(winBgColorEdit_->text());
    }
    if (winWidthSpin_->value() > 0 && winHeightSpin_->value() > 0) {
        // Resize the central canvas viewport for preview purposes
        canvas_->resize(winWidthSpin_->value(), winHeightSpin_->value());
    }
}

void PropertiesPanel::syncFromCanvas()
{
    if (!canvas_ || !windowPage_) return;
    const QSignalBlocker b1(winNameEdit_);
    const QSignalBlocker b2(winWidthSpin_);
    const QSignalBlocker b3(winHeightSpin_);
    const QSignalBlocker b4(winBgColorEdit_);
    isSyncing_ = true;
    winWidthSpin_->setValue(canvas_->width());
    winHeightSpin_->setValue(canvas_->height());
    winBgColorEdit_->setText(canvas_->getBackgroundColorHex());
    isSyncing_ = false;
}

#include "editor/moc_properties_panel.cpp"

