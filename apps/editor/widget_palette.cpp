#include "editor/widget_palette.h"
#include "dashboard/widget_registry.h"

#include <QVBoxLayout>
#include <QDrag>
#include <QMimeData>
#include <QListWidgetItem>

WidgetPalette::WidgetPalette(QWidget* parent)
    : QListWidget(parent)
{

    for (const widget_descriptor_t& descriptor : kWidgetDescriptors)
    {
        auto* entry = new QListWidgetItem(
            QString::fromUtf8(descriptor.friendly_name.data(), static_cast<qsizetype>(descriptor.friendly_name.size())));
        entry->setData(Qt::UserRole,
                       QString::fromUtf8(descriptor.name.data(), static_cast<qsizetype>(descriptor.name.size())));
        addItem(entry);
    }

    setSelectionMode(QAbstractItemView::SingleSelection);
    setDragEnabled(true);
    setDefaultDropAction(Qt::CopyAction);
    setDragDropMode(QAbstractItemView::DragOnly);
    setMouseTracking(true);
    setStyleSheet("QListWidget { border: 0; }");
}

void WidgetPalette::startDrag(Qt::DropActions supportedActions)
{
    QListWidgetItem* item = currentItem();
    if (!item) return;
    auto* drag = new QDrag(this);
    auto* mime = new QMimeData();
    const QString typeKey = item->data(Qt::UserRole).toString();
    mime->setText(typeKey);
    mime->setData("application/x-dashboard-widget", typeKey.toUtf8());
    drag->setMimeData(mime);
    drag->exec(supportedActions, Qt::CopyAction);
}

#include "editor/moc_widget_palette.cpp"
