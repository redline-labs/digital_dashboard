#include "scope/scope_window.h"

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QStatusBar>

#include <spdlog/spdlog.h>

namespace scope
{

ScopeWindow::ScopeWindow(QWidget* parent) : QMainWindow(parent)
{
    setObjectName("ScopeWindow");
    setWindowTitle("Redline Scope");
    resize(1280, 800);

    // Docks may occupy the corners of whichever edges meet there. Without this
    // a left dock and a bottom dock fight over the bottom-left corner and the
    // result depends on which was added first, which makes a restored layout
    // subtly different from the one that was saved.
    setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

    // Nested docks are what make a panel splittable in both directions, which
    // is the whole point of the composable layout.
    setDockNestingEnabled(true);

    empty_hint_ = new QLabel(
        tr("No panels yet.\n\nAdd one from Panels ▸ Add, or press Ctrl+N."), this);
    empty_hint_->setObjectName("empty_hint");
    empty_hint_->setAlignment(Qt::AlignCenter);
    empty_hint_->setStyleSheet("color: palette(mid); font-size: 14px;");
    setCentralWidget(empty_hint_);

    buildMenuBar();
    statusBar()->showMessage(tr("Ready"));
}

ScopeWindow::~ScopeWindow() = default;

void ScopeWindow::buildMenuBar()
{
    QMenu* file_menu = menuBar()->addMenu(tr("&File"));
    file_menu->setObjectName("menu_file");

    QAction* quit = file_menu->addAction(tr("&Quit"));
    quit->setObjectName("action_quit");
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);

    // Panels and View are populated in later milestones; the menus exist now so
    // the window has its final shape and the agent interface can address them.
    QMenu* panels_menu = menuBar()->addMenu(tr("&Panels"));
    panels_menu->setObjectName("menu_panels");

    QMenu* view_menu = menuBar()->addMenu(tr("&View"));
    view_menu->setObjectName("menu_view");
}

}  // namespace scope

#include "scope/moc_scope_window.cpp"
