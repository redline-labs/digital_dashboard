// Driving a modal through the agent interface.
//
// A handler that opens a dialog's exec() or a menu's exec() does not return
// until the modal closes. The dispatcher answers the call as soon as the modal
// opens, and the modal's own event loop keeps serving later calls -- so an
// agent can see the dialog, click its buttons, pick a menu item, and the app
// carries on. Driven from a second thread, as the control socket does.

#include "agent_control/methods.h"
#include "agent_control/server.h"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QMenu>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>

#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

namespace
{

std::atomic<int> g_failures{0};

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

using agent_control::json;

json call(agent_control::AgentServer& server, const std::string& method, json params = json::object())
{
    params["_timeout_ms"] = 3000;
    const json request = {{"id", 1}, {"method", method}, {"params", params}};
    return json::parse(server.handleLine(request.dump()));
}

}  // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    spdlog::set_default_logger(
        std::make_shared<spdlog::logger>("test", std::make_shared<spdlog::sinks::null_sink_mt>()));

    QWidget window;
    window.setObjectName("main_window");
    window.resize(300, 200);
    window.setContextMenuPolicy(Qt::CustomContextMenu);
    window.show();

    agent_control::AgentServer server("test");
    agent_control::registerCoreMethods(server, agent_control::AppInfo{"test", ""});

    // What the modals did, read back once they close.
    int dialog_result = -1;
    bool dialog_returned = false;
    QString menu_choice;

    server.registerMethod(
        "test.open_dialog",
        [&](const json&) -> agent_control::MethodResult
        {
            QDialog dialog(&window);
            dialog.setObjectName("test_dialog");
            auto* layout = new QVBoxLayout(&dialog);
            auto* ok = new QPushButton("OK", &dialog);
            ok->setObjectName("dialog_ok");
            layout->addWidget(ok);
            QObject::connect(ok, &QPushButton::clicked, &dialog, &QDialog::accept);
            dialog_result = dialog.exec();
            dialog_returned = true;
            return json{{"returned", true}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    server.registerMethod("test.state",
                          [&](const json&) -> agent_control::MethodResult
                          {
                              return json{{"dialog_result", dialog_result},
                                          {"dialog_returned", dialog_returned},
                                          {"menu_choice", menu_choice.toStdString()}};
                          });

    QObject::connect(&window, &QWidget::customContextMenuRequested, &window,
                     [&](const QPoint& at)
                     {
                         QMenu menu(&window);
                         menu.setObjectName("test_menu");
                         QAction* first = menu.addAction("First");
                         first->setObjectName("action_first");
                         QAction* second = menu.addAction("Second");
                         second->setObjectName("action_second");
                         QAction* chosen = menu.exec(window.mapToGlobal(at));
                         menu_choice = chosen != nullptr ? chosen->objectName() : QStringLiteral("none");
                     });

    std::thread agent(
        [&]()
        {
            // A dialog: the call that opens it comes back describing it, not
            // timing out.
            const json opened = call(server, "test.open_dialog");
            check(opened.contains("result") && opened["result"].value("modal_opened", false),
                  "the call that opened a dialog is answered with the modal it opened: " + opened.dump());
            check(opened.contains("result") &&
                      opened["result"]["modal"].value("id", std::string()) == "test_dialog",
                  "and names it");

            // While it is open, other calls still run -- inside its event loop.
            const json during = call(server, "test.state");
            check(during.contains("result") && !during["result"].value("dialog_returned", true),
                  "a call made while the dialog is open runs, and the dialog is still open");

            const json snapshot = call(server, "ui.find", json{{"query", "#dialog_ok"}});
            check(snapshot.contains("result"), "the dialog's button can be found: " + snapshot.dump());

            const json clicked = call(server, "input.click", json{{"target", "#dialog_ok"}});
            check(clicked.contains("result"), "its button can be clicked: " + clicked.dump());

            const json after = call(server, "test.state");
            check(after.contains("result") && after["result"].value("dialog_returned", false) &&
                      after["result"].value("dialog_result", -1) == QDialog::Accepted,
                  "and the click closed it, accepted, and the handler carried on: " + after.dump());

            // A context menu: a right click opens it, and an item can be picked.
            const json right = call(server, "input.click",
                                    json{{"target", "#main_window"}, {"button", "right"}, {"pos", {50, 50}}});
            check(right.contains("result") && right["result"].value("modal_opened", false),
                  "a right click opens the context menu, reported as a modal: " + right.dump());

            const json picked = call(server, "input.click", json{{"target", "#test_menu"},
                                                                  {"pos", {20, 35}}});
            check(picked.contains("result"), "the menu can be clicked: " + picked.dump());
            const json chosen = call(server, "test.state");
            check(chosen.contains("result") && chosen["result"].value("menu_choice", std::string()) ==
                                                   "action_second",
                  "and the item under the click was chosen: " + chosen.dump());

            QMetaObject::invokeMethod(&app, &QCoreApplication::quit, Qt::QueuedConnection);
        });

    app.exec();
    agent.join();

    if (g_failures != 0)
    {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures.load());
        return 1;
    }
    return 0;
}
