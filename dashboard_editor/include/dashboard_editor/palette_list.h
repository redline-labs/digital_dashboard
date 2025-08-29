#ifndef DASHBOARD_EDITOR_PALETTE_LIST_H
#define DASHBOARD_EDITOR_PALETTE_LIST_H

#include <QListWidget>

class PaletteList : public QListWidget
{
    Q_OBJECT
public:
    explicit PaletteList(QWidget* parent = nullptr);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
};

#endif // DASHBOARD_EDITOR_PALETTE_LIST_H


