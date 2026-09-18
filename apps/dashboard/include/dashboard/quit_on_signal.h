// SPDX-License-Identifier: GPL-3.0-or-later
//
// Quit the Qt event loop on SIGINT or SIGTERM, without polling.
//
// systemd stops the unit with SIGTERM, and the dashboard used to catch only
// SIGINT -- so on the target it died without tearing down, and at the desk a
// 100 ms timer woke the GUI thread forever to look at a flag. The handler here
// writes the signal number into a pipe (write() is async-signal-safe; spdlog
// and QCoreApplication::quit() are not), and a QSocketNotifier on the other
// end quits from the GUI thread.
#ifndef DASHBOARD_QUIT_ON_SIGNAL_H_
#define DASHBOARD_QUIT_ON_SIGNAL_H_

#include <initializer_list>

class QObject;

namespace dashboard
{

// Install once, after the QCoreApplication exists. `context` owns the
// notifier; the event loop it lives on is the one quit. Each handler fires
// once and then reverts to the default action, so a second signal during a
// teardown that hangs still kills the process. Returns false (and logs) if
// the pipe or a handler could not be set up.
bool quitOnSignals(QObject* context, std::initializer_list<int> signums);

}  // namespace dashboard

#endif  // DASHBOARD_QUIT_ON_SIGNAL_H_
