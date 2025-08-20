#include "palette_list.h"

#include <QDrag>
#include <QMimeData>

PaletteList::PaletteList(QWidget* parent)
    : QListWidget(parent)
{
}

void PaletteList::startDrag(Qt::DropActions supportedActions)
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


