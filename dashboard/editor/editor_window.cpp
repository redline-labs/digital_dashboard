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
#include <QCloseEvent>
#include <QFileDialog>
#include <QKeySequence>
#include <QMessageBox>
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

    propertiesPanel_ = new PropertiesPanel(left);
    auto* properties = propertiesPanel_;
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
    actionLoad->setShortcut(QKeySequence::Open);
    connect(actionLoad, &QAction::triggered, this, &EditorWindow::loadConfig);
    fileMenu->addAction(actionLoad);

    auto* actionSave = new QAction("Save", this);
    actionSave->setShortcut(QKeySequence::Save);
    connect(actionSave, &QAction::triggered, this, &EditorWindow::saveConfig);
    fileMenu->addAction(actionSave);

    auto* editMenu = menuBar()->addMenu("Edit");

    // Neither of these touches the properties panel any more. Undo used to
    // rebuild every widget, so the panel's form always pointed at a frame that
    // no longer existed and had to be blanked. It now keeps the widgets it can
    // and re-emits selectionChanged itself -- with the surviving frame when the
    // selection is still there, with nullptr when it is not -- so blanking here
    // as well would throw away a form the canvas had just refreshed.
    undoAction_ = new QAction("Undo", this);
    undoAction_->setShortcut(QKeySequence::Undo);
    connect(undoAction_, &QAction::triggered, this, [this]
    {
        if (canvas_) canvas_->undo();
    });
    editMenu->addAction(undoAction_);

    redoAction_ = new QAction("Redo", this);
    redoAction_->setShortcut(QKeySequence::Redo);
    connect(redoAction_, &QAction::triggered, this, [this]
    {
        if (canvas_) canvas_->redo();
    });
    editMenu->addAction(redoAction_);

    connect(canvas_, &Canvas::historyChanged, this, &EditorWindow::updateHistoryUi);
    updateHistoryUi();
}

// Keeps the Edit menu and the title bar honest about the document's state.
void EditorWindow::updateHistoryUi()
{
    if (!canvas_)
    {
        return;
    }

    if (undoAction_) undoAction_->setEnabled(canvas_->canUndo());
    if (redoAction_) redoAction_->setEnabled(canvas_->canRedo());

    const QString name = QString::fromStdString(canvas_->windowName());
    setWindowTitle(QString("%1%2 - Dashboard Editor")
                       .arg(name.isEmpty() ? QString("untitled") : name)
                       .arg(canvas_->isDirty() ? " *" : ""));
}

// Nothing used to stand between an unsaved layout and a closed window.
void EditorWindow::closeEvent(QCloseEvent* event)
{
    if (!confirmDiscardChanges("closing"))
    {
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

// Returns false if the user wants to keep what they have. Silent when there is
// nothing to lose, and when there is no one at the screen to ask -- under --mcp
// the editor is driven headlessly and a modal dialog would hang it.
bool EditorWindow::confirmDiscardChanges(const QString& action)
{
    if (!canvas_ || !canvas_->isDirty())
    {
        return true;
    }

    if (headless_)
    {
        SPDLOG_WARN("Discarding unsaved editor changes on {} (headless: nobody to ask).",
                    action.toStdString());
        return true;
    }

    const auto choice = QMessageBox::warning(
        this, "Unsaved changes",
        QString("This layout has unsaved changes.\n\nSave before %1?").arg(action),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Save);

    if (choice == QMessageBox::Cancel)
    {
        return false;
    }
    if (choice == QMessageBox::Save)
    {
        saveConfig();
        // A failed or cancelled save must not fall through into discarding.
        return !canvas_->isDirty();
    }
    return true;
}

bool EditorWindow::loadConfigFrom(const QString& path)
{
    if (path.isEmpty())
    {
        return false;
    }

    SPDLOG_INFO("Loading dashboard config from: {}", path.toStdString());
    auto cfg = load_app_config(path.toStdString());
    if (!cfg)
    {
        SPDLOG_ERROR("Config failed to load: {}", path.toStdString());
        return false;
    }

    if (!canvas_)
    {
        return false;
    }

    canvas_->loadFromAppConfig(cfg.value());

    // A freshly loaded document is clean, and its history starts here -- undoing
    // past a load into the previous document would be nonsense.
    canvas_->clearHistory();
    canvas_->markSaved();

    statusBar()->showMessage(QString("Loaded '%1' (%2x%3)")
                             .arg(QString::fromStdString(cfg.value().name))
                             .arg(cfg.value().width)
                             .arg(cfg.value().height), 3000);
    if (propertiesPanel_)
    {
        // Clear the selection first: the panel's form points at frames the load
        // has just destroyed.
        propertiesPanel_->setSelectedWidget(nullptr);
        propertiesPanel_->syncFromCanvas();
    }
    updateHistoryUi();
    return true;
}

bool EditorWindow::saveConfigTo(const QString& path)
{
    if (!canvas_ || path.isEmpty())
    {
        return false;
    }

    // Export current canvas to a single-window app config. The name comes from
    // the canvas, which carries the one the config was loaded with -- this used
    // to be hardcoded to "editor_window", so load-then-save renamed every
    // config it touched.
    app_config_t app = canvas_->exportAppConfig();

    try
    {
        YAML::Node node = YAML::convert<app_config_t>::encode(app);
        YAML::Emitter emitter;
        emitter << node;

        // Check the stream, both at open and after writing. This reported
        // success unconditionally, so saving to a path the process cannot write
        // showed "Saved config to ..." and dropped the layout on the floor.
        std::ofstream ofs(path.toStdString());
        if (!ofs)
        {
            SPDLOG_ERROR("Failed to open '{}' for writing.", path.toStdString());
            statusBar()->showMessage(QString("Could not write %1").arg(path), 5000);
            return false;
        }

        ofs << emitter.c_str();
        ofs.close();
        if (!ofs)
        {
            SPDLOG_ERROR("Failed to write '{}'.", path.toStdString());
            statusBar()->showMessage(QString("Could not write %1").arg(path), 5000);
            return false;
        }

        canvas_->markSaved();
        updateHistoryUi();
        statusBar()->showMessage(QString("Saved config to %1").arg(path), 3000);
        SPDLOG_INFO("Saved dashboard config to: {}", path.toStdString());
        return true;
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Failed to save config: {}", e.what());
        return false;
    }
}

void EditorWindow::loadConfig()
{
    if (!confirmDiscardChanges("opening another layout"))
    {
        return;
    }

    const QString path = QFileDialog::getOpenFileName(this,
                                                      "Open Dashboard Config",
                                                      "",
                                                      "YAML Files (*.yaml *.yml)");
    loadConfigFrom(path);
}

void EditorWindow::saveConfig()
{
    const QString path = QFileDialog::getSaveFileName(this,
                                                        "Save Dashboard Config",
                                                        "untitled.yaml",
                                                        "YAML Files (*.yaml *.yml)");
    saveConfigTo(path);
}


#include "editor/moc_editor_window.cpp"
