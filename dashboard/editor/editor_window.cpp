#include "editor/editor_window.h"
#include "editor/widget_palette.h"
#include "editor/properties_panel.h"
#include "editor/canvas.h"

#include <QSplitter>
#include <QListView>
#include <QStringListModel>
#include <QToolBar>
#include <QStatusBar>
#include <QLabel>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QAction>
#include <QToolButton>
#include <QMenuBar>
#include <QFileDialog>
#include "spdlog/spdlog.h"
#include "dashboard/app_config.h"

#include <yaml-cpp/yaml.h>
#include <fstream>

EditorWindow::EditorWindow(QWidget* parent) :
  QMainWindow(parent),
  widgetPalette_(nullptr),
  canvas_(nullptr),
  toggleInterceptAction_(nullptr)
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

    // Add a toggle to control whether the canvas intercepts interactions
    toggleInterceptAction_ = new QAction(this);
    toggleInterceptAction_->setCheckable(true);
    toggleInterceptAction_->setChecked(true);
    toggleInterceptAction_->setText("Editor Mode");
    toggleInterceptAction_->setToolTip("When enabled, clicks/drags are handled by the editor (selection & resize). When disabled, events pass through to the widget.");

    auto* toggleBtn = new QToolButton(this);
    toggleBtn->setDefaultAction(toggleInterceptAction_);
    toggleBtn->setCheckable(true);
    toggleBtn->setChecked(true);
    toggleBtn->setAutoRaise(true);
    toggleBtn->setStyleSheet("QToolButton:checked{ background: #2da44e; color: white; border-radius: 4px; padding: 2px 6px;} QToolButton{ padding: 2px 6px; }");

    statusBar()->addPermanentWidget(toggleBtn);

    connect(toggleInterceptAction_, &QAction::toggled, this, [this](bool on)
    {
        if (canvas_)
        {
            canvas_->setEditorMode(on);
        }
    });

    buildMenuBar();
}

void EditorWindow::buildMenuBar()
{
    auto* fileMenu = menuBar()->addMenu("File");

    auto* actionLoad = new QAction("Load", this);
    connect(actionLoad, &QAction::triggered, this, &EditorWindow::loadConfig);
    fileMenu->addAction(actionLoad);

    auto* actionSave = new QAction("Save", this);
    connect(actionSave, &QAction::triggered, this, &EditorWindow::saveConfig);
    fileMenu->addAction(actionSave);
}

void EditorWindow::loadConfig()
{
    QString startDir = QString::fromUtf8("/Users/ryan/src/mercedes_dashboard/configs/dashboard");
    const QString path = QFileDialog::getOpenFileName(this,
                                                      "Open Dashboard Config",
                                                      startDir,
                                                      "YAML Files (*.yaml *.yml)");
    if (path.isEmpty())
    {
        return;
    }
    SPDLOG_INFO("Loading dashboard config from: {}", path.toStdString());
    auto cfg = load_app_config(path.toStdString());
    if (!cfg)
    {
        SPDLOG_ERROR("Config failed to load: {}", path.toStdString());
        return;
    }

    if (canvas_)
    {
        canvas_->loadFromAppConfig(cfg.value());
        statusBar()->showMessage(QString("Loaded '%1' (%2x%3)")
                                 .arg(QString::fromStdString(cfg.value().name))
                                 .arg(cfg.value().width)
                                 .arg(cfg.value().height), 3000);
    }
}

void EditorWindow::saveConfig()
{
    if (!canvas_) return;
    // Ask for a filename in configs/dashboard
    QString startDir = QString::fromUtf8("/Users/ryan/src/mercedes_dashboard/configs/dashboard");
    const QString path = QFileDialog::getSaveFileName(this,
                                                        "Save Dashboard Config",
                                                        startDir + "/untitled.yaml",
                                                        "YAML Files (*.yaml *.yml)");
    if (path.isEmpty()) return;

    // Export current canvas to a single-window app config
    app_config_t app = canvas_->exportAppConfig("editor_window");

    try
    {
        YAML::Node node = YAML::convert<app_config_t>::encode(app);
        YAML::Emitter emitter;
        emitter << node;
        std::ofstream ofs(path.toStdString());
        ofs << emitter.c_str();
        ofs.close();
        statusBar()->showMessage(QString("Saved config to %1").arg(path), 3000);
        SPDLOG_INFO("Saved dashboard config to: {}", path.toStdString());
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Failed to save config: {}", e.what());
    }
}


#include "editor/moc_editor_window.cpp"
