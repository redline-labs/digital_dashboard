#include "editor_window.h"

#include <QSplitter>
#include <QListView>
#include <QStringListModel>
#include <QToolBar>
#include <QStatusBar>
#include <QLabel>
#include <QVBoxLayout>
#include <QScrollArea>

#include "widget_palette.h"
#include "properties_panel.h"
#include "canvas.h"

EditorWindow::EditorWindow(QWidget* parent)
    : QMainWindow(parent), widgetPalette_(nullptr), canvas_(nullptr)
{
    auto* splitter = new QSplitter(this);

    // Left panel: palette + properties in a vertical splitter (adjustable divider)
    auto* left = new QSplitter(Qt::Vertical, splitter);
    widgetPalette_ = new WidgetPalette(left);

    auto* properties = new PropertiesPanel(left);
    auto* propertiesScroll = new QScrollArea(left);
    propertiesScroll->setWidget(properties);
    propertiesScroll->setWidgetResizable(true);
    propertiesScroll->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    propertiesScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    propertiesScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    left->addWidget(widgetPalette_);
    left->addWidget(propertiesScroll);
    left->setStretchFactor(0, 0);
    left->setStretchFactor(1, 1);

    canvas_ = new Canvas(nullptr);
    canvas_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto* scrollArea = new QScrollArea(splitter);
    scrollArea->setWidget(canvas_);
    scrollArea->setWidgetResizable(false);
    scrollArea->setAlignment(Qt::AlignLeft | Qt::AlignTop);

    splitter->addWidget(left);
    splitter->addWidget(scrollArea);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);

    setCentralWidget(splitter);

    // Bridge selection to properties
    properties->setCanvas(canvas_);
    connect(canvas_, &Canvas::selectionChanged, properties, &PropertiesPanel::setSelectedWidget);

    auto* tb = addToolBar("Main");
    tb->setMovable(false);
    statusBar()->showMessage("Drag widgets from the left onto the canvas");
}


