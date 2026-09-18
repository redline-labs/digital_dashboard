// SPDX-License-Identifier: GPL-3.0-or-later
//
// SIGTERM and SIGINT both end the event loop, and a second one is not caught.
//
// SIGTERM is the case that matters: it is what systemd sends, and the
// dashboard used to handle only SIGINT, so a stopped unit skipped its
// teardown. A watchdog turns "the loop never quit" into a failure instead of a
// hung test.
#include "dashboard/quit_on_signal.h"

#include <QCoreApplication>
#include <QTimer>

#include <csignal>
#include <cstdio>
#include <string>

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// Raises `signum` from inside the loop and reports whether the loop then
// quit on its own, before the watchdog.
bool loopQuitsOn(QCoreApplication& app, int signum)
{
    bool timed_out = false;
    QTimer::singleShot(0, [signum]() { std::raise(signum); });
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [&]() {
        timed_out = true;
        QCoreApplication::quit();
    });
    watchdog.start(2000);
    app.exec();
    return !timed_out;
}

bool isDefault(int signum)
{
    struct sigaction current {};
    ::sigaction(signum, nullptr, &current);
    return current.sa_handler == SIG_DFL;
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    check(dashboard::quitOnSignals(&app, {SIGINT, SIGTERM}), "installs");
    check(!isDefault(SIGTERM) && !isDefault(SIGINT), "and owns both signals");

    check(loopQuitsOn(app, SIGTERM), "SIGTERM -- what systemd sends -- quits the loop");
    check(isDefault(SIGTERM), "and the next SIGTERM gets the default action, so a hung teardown can be killed");
    check(!isDefault(SIGINT), "without giving up SIGINT");

    check(loopQuitsOn(app, SIGINT), "SIGINT quits the loop too");

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
