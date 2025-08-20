#ifndef DASHBOARD_EDITOR_WIDGET_PALETTE_H
#define DASHBOARD_EDITOR_WIDGET_PALETTE_H

#include <QWidget>
#include <QStringList>

class PaletteList;

class WidgetPalette : public QWidget
{
    Q_OBJECT
public:
    explicit WidgetPalette(QWidget* parent = nullptr);

private:
    PaletteList* list_;
};

#endif // DASHBOARD_EDITOR_WIDGET_PALETTE_H


