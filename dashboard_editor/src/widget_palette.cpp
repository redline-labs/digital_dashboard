#include "widget_palette.h"

#include "palette_list.h"
#include <QVBoxLayout>
#include <QDrag>
#include <QMimeData>

namespace {
// Map of available widget types displayed in the palette.
// The keys are user-friendly labels; the values are internal type keys.
struct PaletteItem { const char* label; const char* typeKey; };
constexpr PaletteItem kItems[] = {
    {"Static Text", "static_text"},
    {"Value Readout", "value_readout"},
    {"Mercedes 190E Speedometer", "mercedes_190e_speedometer"},
    {"Mercedes 190E Tachometer", "mercedes_190e_tachometer"},
    {"Mercedes 190E Cluster Gauge", "mercedes_190e_cluster_gauge"},
    {"Sparkline", "sparkline"},
    {"Background Rect", "background_rect"},
    {"Mercedes 190E Telltale", "mercedes_190e_telltale"},
    {"MoTeC C125 Tachometer", "motec_c125_tachometer"},
    {"MoTeC CDL3 Tachometer", "motec_cdl3_tachometer"},
    {"CarPlay", "carplay"}
};
}

WidgetPalette::WidgetPalette(QWidget* parent)
    : QWidget(parent), list_(new PaletteList(this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(list_);
    setLayout(layout);

    for (const auto& item : kItems)
    {
        auto* entry = new QListWidgetItem(QString::fromUtf8(item.label));
        entry->setData(Qt::UserRole, QString::fromUtf8(item.typeKey));
        list_->addItem(entry);
    }

    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setDragEnabled(true);
    list_->setDefaultDropAction(Qt::CopyAction);

    // Provide custom drag so we can set mime data to communicate the type key
    list_->setDragDropMode(QAbstractItemView::DragOnly);
    list_->setMouseTracking(true);

    list_->setStyleSheet("QListWidget { border: 0; }");

    // Reimplement startDrag via event filter approach by subclassing QListWidget is cleaner; but to keep things simple, we rely on Qt's
    // default which packs the selected text. The Canvas will accept both text/plain and our custom format.
}


