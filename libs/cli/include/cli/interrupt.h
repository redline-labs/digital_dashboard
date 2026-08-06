#ifndef CLI_INTERRUPT_H_
#define CLI_INTERRUPT_H_

namespace cli
{

// Ctrl-C, once, for every verb that runs until told to stop.
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
void installInterruptHandler();

// False until SIGINT arrives. Poll it from a loop:
//
//     while (!cli::interrupted()) { ... }
//
// A verb that never calls installInterruptHandler() sees this stay false
// forever, which is the right behaviour for one that exits on its own.
bool interrupted();

}  // namespace cli

#endif  // CLI_INTERRUPT_H_
