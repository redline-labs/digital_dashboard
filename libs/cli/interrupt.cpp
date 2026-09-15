#include "cli/interrupt.h"

#include <csignal>

namespace cli
{

namespace
{

// volatile sig_atomic_t, not std::atomic<bool>: this is the only type the
// standard permits a signal handler to write to. See the header.
volatile std::sig_atomic_t g_interrupted = 0;

extern "C" void handleInterrupt(int /*signum*/)
{
    g_interrupted = 1;
}

}  // namespace

void installInterruptHandler()
{
    std::signal(SIGINT, handleInterrupt);
    // SIGTERM too: it is how systemd stops a unit, and a node that only handled
    // Ctrl-C was killed mid-write rather than shutting down.
    std::signal(SIGTERM, handleInterrupt);
}

bool interrupted()
{
    return g_interrupted != 0;
}

}  // namespace cli
