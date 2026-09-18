#include "cli/interrupt.h"

#include <poll.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <mutex>

namespace cli
{

namespace
{

// volatile sig_atomic_t, not std::atomic<bool>: this is the only type the
// standard permits a signal handler to write to. See the header.
volatile std::sig_atomic_t g_interrupted = 0;

// The self-pipe waitForInterrupt() sleeps on. -1 until created, and if it
// cannot be, the wait degrades to noticing the flag once a period.
volatile std::sig_atomic_t g_wake_read = -1;
volatile std::sig_atomic_t g_wake_write = -1;

extern "C" void handleInterrupt(int /*signum*/)
{
    g_interrupted = 1;
    const int fd = g_wake_write;
    if (fd >= 0)
    {
        // write() is async-signal-safe; errno is saved because the thread this
        // interrupted may be about to read it.
        const int saved = errno;
        const char byte = 1;
        [[maybe_unused]] const ssize_t written = ::write(fd, &byte, 1);
        errno = saved;
    }
}

void makeWakePipe()
{
    int fds[2];
    if (::pipe(fds) != 0)
    {
        return;
    }
    for (const int fd : fds)
    {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        // Non-blocking so a handler never blocks on a full pipe; nothing drains
        // it, because once interrupted there is nothing left to wait for.
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    g_wake_read = fds[0];
    g_wake_write = fds[1];
}

}  // namespace

void installInterruptHandler()
{
    // Before the handlers, so a signal cannot find the pipe half made.
    static std::once_flag pipe_once;
    std::call_once(pipe_once, makeWakePipe);

    std::signal(SIGINT, handleInterrupt);
    // SIGTERM too: it is how systemd stops a unit, and a node that only handled
    // Ctrl-C was killed mid-write rather than shutting down.
    std::signal(SIGTERM, handleInterrupt);
}

bool interrupted()
{
    return g_interrupted != 0;
}

void waitForInterrupt(const std::function<void()>& tick, std::chrono::milliseconds period)
{
    installInterruptHandler();
    const int timeout_ms = static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(period.count(), 1, 60'000));

    while (!interrupted())
    {
        if (tick)
        {
            tick();
        }
        if (interrupted())
        {
            break;
        }
        // A negative fd is ignored by poll(), which makes this a plain sleep
        // when the pipe could not be made. EINTR just goes round the loop.
        pollfd wake{.fd = g_wake_read, .events = POLLIN, .revents = 0};
        ::poll(&wake, 1, timeout_ms);
    }
}

}  // namespace cli
