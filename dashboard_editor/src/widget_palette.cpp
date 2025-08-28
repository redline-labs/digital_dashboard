#include "widget_palette.h"

#include "palette_list.h"
#include "widget_registry.h"
#include <QVBoxLayout>
#include <QDrag>
#include <QMimeData>

namespace {}

WidgetPalette::WidgetPalette(QWidget* parent)
    : QWidget(parent), list_(new PaletteList(this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(list_);
    setLayout(layout);

    for (const auto& info : widget_registry::kAllWidgets)
    {
        auto* entry = new QListWidgetItem(QString::fromUtf8(info.label));
        const std::string_view type_name = reflection::enum_traits<widget_type_t>::to_string(info.type);
        entry->setData(Qt::UserRole, QString::fromUtf8(type_name.data(), static_cast<int>(type_name.size())));
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


