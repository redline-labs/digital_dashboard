#include "properties_panel.h"

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

#include "static_text/static_text.h"
#include "background_rect/background_rect.h"
#include "mercedes_190e_cluster_gauge/mercedes_190e_cluster_gauge.h"
#include "mercedes_190e_speedometer/mercedes_190e_speedometer.h"
#include "mercedes_190e_tachometer/mercedes_190e_tachometer.h"
#include "motec_c125_tachometer/motec_c125_tachometer.h"
#include "motec_cdl3_tachometer/motec_cdl3_tachometer.h"
#include "sparkline/sparkline.h"
#include "value_readout/value_readout.h"
//#include "carplay/carplay.h"

#include "reflection/reflection.h"
#include "spdlog/spdlog.h"

#include "canvas.h"
#include "editor_constants.h"

#include <string>
#include <vector>

PropertiesPanel::PropertiesPanel(QWidget* parent)
    : QWidget(parent), selected_(nullptr), stack_(new QStackedWidget(this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
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
        winWidthSpin_->setValue(canvas_->width());
        winHeightSpin_->setValue(canvas_->height());
        // Set bg color field from palette if desired
        const QColor c = canvas_->palette().color(QPalette::Window);
        winBgColorEdit_->setText(c.name());
    }
}

namespace
{
    template <typename T>
    QWidget* createLeafEditor(QWidget* parent, std::string_view fieldName, std::string_view typeName)
    {
        using FieldType = std::decay_t<T>;

        if constexpr (std::is_same_v<FieldType, std::string>)
        {
            auto* line = new QLineEdit(parent);
            return line;
        }
        else if constexpr (std::is_enum_v<FieldType>)
        {
            auto* combo = new QComboBox(parent);
            for (const auto& value : reflection::enum_traits<FieldType>::values())
            {
                const std::string_view value_str = reflection::enum_traits<FieldType>::to_string(value);
                combo->addItem(QString::fromUtf8(value_str.data(), static_cast<int>(value_str.size())));
            }
            return combo;
        }
        else if constexpr (std::is_same_v<FieldType, bool>)
        {
            auto* check = new QCheckBox(parent);
            return check;
        }
        else if constexpr (std::is_integral_v<FieldType>)
        {
            auto* spin = new QSpinBox(parent);
            return spin;
        }
        else if constexpr (std::is_floating_point_v<FieldType>)
        {
            auto* dspin = new QDoubleSpinBox(parent);
            dspin->setDecimals(3);
            return dspin;
        }
        else
        {
            auto* line = new QLineEdit(parent);
            line->setReadOnly(true);
            line->setPlaceholderText("(unsupported type)");
            SPDLOG_WARN("Unsupported type: '{}' for field '{}'", typeName, fieldName);
            return line;
        }
    }

    template <typename T>
    QWidget* createEditorFor(QWidget* parent, std::string_view fieldName, T& ref, std::string_view typeName)
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
            itemsLayout->setContentsMargins(0,0,0,0);
            itemsLayout->setSpacing(4);

            auto addRow = [items, itemsLayout, fieldName, typeName]()
            {
                const int idx = itemsLayout->count();
                auto* row = new QWidget(items);
                auto* h = new QHBoxLayout(row);
                h->setContentsMargins(0,0,0,0);
                h->setSpacing(6);
                auto* idxLabel = new QLabel(QString("[%1]").arg(idx), row);
                h->addWidget(idxLabel);
                auto* childEditor = createLeafEditor<Elem>(row, fieldName, typeName);
                h->addWidget(childEditor, 1);
                row->setLayout(h);
                itemsLayout->addWidget(row);
            };

            // Populate existing elements
            for (auto& elem : ref)
            {
                (void)elem;
                addRow();
            }

            // Wire buttons
            QObject::connect(addBtn, &QPushButton::clicked, container, [addRow]{ addRow(); });
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
        else
        {
            return createLeafEditor<FieldType>(parent, fieldName, typeName);
        }
    }

    template <typename Config>
    QWidget* buildFormFromConfig(QWidget* parent)
    {
        auto* page = new QWidget(parent);
        auto* form = new QFormLayout(page);
        Config cfg{};
        reflection::visit_fields<Config>(cfg, [&](std::string_view name, auto& ref, std::string_view type)
        {
            QString label = QString::fromStdString(name.data());
            QWidget* editor = createEditorFor(page, name, ref, type);
            form->addRow(label, editor);
        });

        page->setLayout(form);
        return page;
    }
}

void PropertiesPanel::buildWindowPage()
{
    if (windowPage_) return;
    windowPage_ = new QWidget(this);
    auto* form = new QFormLayout(windowPage_);
    winNameEdit_ = new QLineEdit(windowPage_);
    winWidthSpin_ = new QSpinBox(windowPage_);
    winWidthSpin_->setRange(100, 10000);
    winHeightSpin_ = new QSpinBox(windowPage_);
    winHeightSpin_->setRange(100, 10000);
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
            winWidthSpin_->setValue(canvas_->width());
            winHeightSpin_->setValue(canvas_->height());
            const QColor c = canvas_->palette().color(QPalette::Window);
            winBgColorEdit_->setText(c.name());
        }
        return;
    }

    // Registry of supported widgets at compile time
    struct Entry { const char* className; std::function<QWidget*(PropertiesPanel*)> builder; };
    static const Entry registry[] = {
        { "StaticTextWidget", [](PropertiesPanel* self){ return buildFormFromConfig<StaticTextWidget::config_t>(self); } },
        { "BackgroundRectWidget", [](PropertiesPanel* self){ return buildFormFromConfig<BackgroundRectWidget::config_t>(self); } },
        { "Mercedes190EClusterGauge", [](PropertiesPanel* self){ return buildFormFromConfig<Mercedes190EClusterGauge::config_t>(self); } },
        { "Mercedes190ESpeedometer", [](PropertiesPanel* self){ return buildFormFromConfig<Mercedes190ESpeedometer::config_t>(self); } },
        { "Mercedes190ETachometer", [](PropertiesPanel* self){ return buildFormFromConfig<Mercedes190ETachometer::config_t>(self); } },
        { "MotecC125Tachometer", [](PropertiesPanel* self){ return buildFormFromConfig<MotecC125Tachometer::config_t>(self); } },
        { "MotecCdl3Tachometer", [](PropertiesPanel* self){ return buildFormFromConfig<MotecCdl3Tachometer::config_t>(self); } },
        //{ "Sparkline", [](PropertiesPanel* self){ return buildFormFromConfig<Sparkline::config_t>(self); } },
        //{ "ValueReadout", [](PropertiesPanel* self){ return buildFormFromConfig<ValueReadout::config_t>(self); } },
        //{ "Carplay", [](PropertiesPanel* self){ return buildFormFromConfig<Carplay::config_t>(self); } },
        // Add more mappings here as widgets adopt REFLECT_STRUCT for their config
    };

    const QString qtClass = w->metaObject()->className();
    if (widgetPages_.contains(qtClass))
    {
        stack_->setCurrentWidget(widgetPages_.value(qtClass));
        return;
    }

    for (const auto& e : registry)
    {
        if (qtClass == e.className)
        {
            QWidget* page = e.builder(this);
            widgetPages_.insert(qtClass, page);
            stack_->addWidget(page);
            stack_->setCurrentWidget(page);
            return;
        }
    }

    // Other types unsupported for now
    showUnsupported(w->metaObject()->className());
    // leave as unsupported page
}

// Removed per reflect-first UI goal; wiring updates will follow later.

void PropertiesPanel::applyWindowEdits()
{
    if (!canvas_) return;
    // Apply background immediately; name/size can be used for serialization later
    if (!winBgColorEdit_->text().isEmpty()) {
        canvas_->setBackgroundColor(winBgColorEdit_->text());
    }
    if (winWidthSpin_->value() > 0 && winHeightSpin_->value() > 0) {
        // Resize the central canvas viewport for preview purposes
        canvas_->resize(winWidthSpin_->value(), winHeightSpin_->value());
    }
}


