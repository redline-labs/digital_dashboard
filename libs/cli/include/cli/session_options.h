#ifndef CLI_SESSION_OPTIONS_H_
#define CLI_SESSION_OPTIONS_H_

#include <cxxopts.hpp>

namespace cli
{

// Turns --connect and --mode into pub_sub::SessionManager config overrides.
//
// Called by Program::run() before any verb runs, which is the only correct
// moment: SessionManager caches one session process-wide and insertConfig()
// affects "the next session opened, not one that is already running", so an
// override applied after a verb's first getOrCreate() would be silently
// ignored.
//
// Until now nothing in the tree called insertConfig() outside a test, so no tool
// could be pointed at a router -- every one of them was hardwired to peer-mode
// discovery on whatever network it happened to be on.
//
// Declared here rather than in program.h so that only this translation unit
// includes <zenoh.hxx>. That header costs ~89,000 preprocessed lines, and every
// verb in every tool includes program.h.
void applySessionOverrides(const cxxopts::ParseResult& parsed);

}  // namespace cli

#endif  // CLI_SESSION_OPTIONS_H_
