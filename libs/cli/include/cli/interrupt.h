#ifndef CLI_INTERRUPT_H_
#define CLI_INTERRUPT_H_

#include <chrono>
#include <functional>

namespace cli
{

// Ctrl-C and SIGTERM, once, for every verb and node that runs until told to
// stop. SIGTERM is what systemd sends to stop a unit.
//
// This replaces three near-identical copies in nodes/inspect -- `g_running_dump`,
// `g_running_info`, `g_running_hz`, each with its own file-scope atomic and its
// own handler function. They were identical apart from the names, and a fourth
// verb would have added a fourth.
//
// Only a `volatile std::sig_atomic_t` may be written from a signal handler; a
// std::atomic<bool> is not guaranteed to be async-signal-safe (it may be
// lock-free in practice, but "in practice" is not what the standard says, and
// the whole point of this file is that the answer is written down once).
//
// Safe to call more than once.
void installInterruptHandler();

// False until SIGINT or SIGTERM arrives. Poll it from a loop:
//
//     while (!cli::interrupted()) { ... }
//
// A verb that never calls installInterruptHandler() sees this stay false
// forever, which is the right behaviour for one that exits on its own.
bool interrupted();

// A node's main loop: calls `tick` at once and then every `period` until SIGINT
// or SIGTERM, and returns as soon as one arrives rather than at the end of the
// period. Installs the handler if nothing has yet.
//
//     cli::waitForInterrupt([&] { health.kick(); });
//
// The handler writes to a pipe this sleeps on, so the wake does not depend on
// which thread the kernel hands the signal to -- zenoh's threads do not block
// it, and a sigtimedwait() here would have required every thread to.
void waitForInterrupt(const std::function<void()>& tick,
                      std::chrono::milliseconds period = std::chrono::seconds(1));

}  // namespace cli

#endif  // CLI_INTERRUPT_H_
