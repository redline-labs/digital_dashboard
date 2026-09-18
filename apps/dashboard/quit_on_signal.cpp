// SPDX-License-Identifier: GPL-3.0-or-later
#include "dashboard/quit_on_signal.h"

#include <QCoreApplication>
#include <QSocketNotifier>

#include <spdlog/spdlog.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <unistd.h>

namespace dashboard
{
namespace
{

// The write end, for the handler. A plain int: the handler may read nothing
// that is not lock-free, and it is set before any handler is installed.
volatile std::sig_atomic_t g_write_fd = -1;

extern "C" void onSignal(int signum)
{
    // Preserve errno: the handler interrupts arbitrary code that may be
    // about to read it.
    const int saved = errno;
    const auto byte = static_cast<unsigned char>(signum);
    [[maybe_unused]] const ssize_t ignored = ::write(g_write_fd, &byte, 1);
    errno = saved;
}

std::string signalName(int signum)
{
    switch (signum)
    {
        case SIGINT: return "SIGINT";
        case SIGTERM: return "SIGTERM";
        default: return "Signal " + std::to_string(signum);
    }
}

}  // namespace

bool quitOnSignals(QObject* context, std::initializer_list<int> signums)
{
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
    {
        SPDLOG_ERROR("quitOnSignals: pipe2 failed: {}", std::strerror(errno));
        return false;
    }
    g_write_fd = fds[1];

    auto* notifier = new QSocketNotifier(fds[0], QSocketNotifier::Read, context);
    QObject::connect(notifier, &QSocketNotifier::activated, notifier, [fd = fds[0]]()
    {
        unsigned char byte = 0;
        while (::read(fd, &byte, 1) == 1)
        {
            SPDLOG_WARN("{} received, quitting.", signalName(byte));
        }
        QCoreApplication::quit();
    });

    struct sigaction action {};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    // SA_RESETHAND: one polite request, then the default action. SA_RESTART:
    // a read() or poll() elsewhere is not handed an EINTR it never expected.
    // sa_flags is an int and glibc spells SA_RESETHAND as 0x80000000u; the
    // bit pattern is what matters.
    action.sa_flags = static_cast<int>(SA_RESETHAND | SA_RESTART);
    for (const int signum : signums)
    {
        if (::sigaction(signum, &action, nullptr) != 0)
        {
            SPDLOG_ERROR("quitOnSignals: sigaction({}) failed: {}", signum, std::strerror(errno));
            return false;
        }
    }
    return true;
}

}  // namespace dashboard
