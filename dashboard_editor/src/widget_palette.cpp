#include "dashboard_editor/widget_palette.h"
#include "dashboard_editor/widget_registry.h"

#include <QVBoxLayout>
#include <QDrag>
#include <QMimeData>
#include <QListWidgetItem>

WidgetPalette::WidgetPalette(QWidget* parent)
    : QListWidget(parent)
{
    for (const auto& info : widget_registry::kAllWidgets)
    {
        auto* entry = new QListWidgetItem(QString::fromUtf8(info.label));
        const std::string_view type_name = reflection::enum_traits<widget_type_t>::to_string(info.type);
        entry->setData(Qt::UserRole, QString::fromUtf8(type_name.data(), static_cast<int>(type_name.size())));
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

#include "dashboard_editor/moc_widget_palette.cpp"
