// m150_sim: the MoTeC M150, emulated, running an image built outside the tree (a kernel and an M1 application).
//
//   m150_sim serve [--image IMAGE] [-c CONFIG] [--stepped]   its CAN ports on zenoh (CAN1 on can0 by default)
//   m150_sim run IMAGE [--seconds S] [--candump-in FILE]      the console on stderr, every frame on stdout (candump)
#include "verbs.h"

#include <array>

#include "cli/program.h"

namespace {

constexpr std::array<cli::Verb, 2> kVerbs{{
    {"serve", "Run the M150 on the bus, its CAN on zenoh, for agents and nodes", m150::cli::addServeOptions,
     m150::cli::runServe},
    {"run", "Run an image for a while and print its console and CAN frames", m150::cli::addRunOptions,
     m150::cli::runRun},
}};

}  // namespace

int main(int argc, char** argv) {
    const cli::Program program("m150_sim", "An emulated MoTeC M150 running an image built outside the tree", kVerbs);
    return program.run(argc, argv);
}
