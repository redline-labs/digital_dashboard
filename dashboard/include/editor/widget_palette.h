#ifndef DASHBOARD_EDITOR_WIDGET_PALETTE_H
#define DASHBOARD_EDITOR_WIDGET_PALETTE_H

#include <QListWidget>

class WidgetPalette : public QListWidget
{
    Q_OBJECT
public:
    explicit WidgetPalette(QWidget* parent = nullptr);

protected:
    void startDrag(Qt::DropActions supportedActions) override;
};

#endif // DASHBOARD_EDITOR_WIDGET_PALETTE_H


