#include "editor/widget_palette.h"
#include "editor/widget_registry.h"

#include <QVBoxLayout>
#include <QDrag>
#include <QMimeData>
#include <QListWidgetItem>

WidgetPalette::WidgetPalette(QWidget* parent)
    : QListWidget(parent)
{

// Add each of the available widgets to the palette.
 #define WIDGET_INFO_ENTRY(widget_class) \
    { \
        auto* entry = new QListWidgetItem(QString::fromUtf8(widget_class::kFriendlyName)); \
        const std::string_view type_name = reflection::enum_to_string(widget_class::kWidgetType); \
        entry->setData(Qt::UserRole, QString::fromUtf8(type_name.data(), static_cast<int>(type_name.size()))); \
        addItem(entry); \
    }

    FOR_EACH_WIDGET(WIDGET_INFO_ENTRY)
#undef WIDGET_INFO_ENTRY

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
