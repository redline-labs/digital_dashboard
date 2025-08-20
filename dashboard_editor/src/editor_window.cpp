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

    // Left panel: palette + properties
    auto* left = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0,0,0,0);
    widgetPalette_ = new WidgetPalette(left);
    auto* properties = new PropertiesPanel(left);
    leftLayout->addWidget(widgetPalette_);
    leftLayout->addWidget(properties);
    left->setLayout(leftLayout);

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


