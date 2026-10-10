// SPDX-License-Identifier: GPL-3.0-or-later
//
// Quit the Qt event loop on SIGINT or SIGTERM, without polling.
//
// systemd stops a unit with SIGTERM, and the apps used to catch only SIGINT --
// so on the target they died without tearing down, and at the desk a 100 ms
// timer woke the GUI thread forever to look at a flag. Every GUI app installs
// this. The handler here
// writes the signal number into a pipe (write() is async-signal-safe; spdlog
// and QCoreApplication::quit() are not), and a QSocketNotifier on the other
// end quits from the GUI thread.
#ifndef QT_HELPERS_QUIT_ON_SIGNAL_H_
#define QT_HELPERS_QUIT_ON_SIGNAL_H_

#include <initializer_list>

class QObject;

namespace qt_helpers
{

// A signal is not a request a window can refuse: the loop is ended with
// QCoreApplication::exit(), so no close event runs and nothing unsaved is
// offered for saving.
//
// Install once, after the QCoreApplication exists. `context` owns the
// notifier; the event loop it lives on is the one quit. Each handler fires
// once and then reverts to the default action, so a second signal during a
// teardown that hangs still kills the process. Returns false (and logs) if
// the pipe or a handler could not be set up.
bool quitOnSignals(QObject* context, std::initializer_list<int> signums);

}  // namespace qt_helpers

#endif  // QT_HELPERS_QUIT_ON_SIGNAL_H_
