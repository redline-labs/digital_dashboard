// m150_sim: the M150, emulated.
//
//   m150_sim run IMAGE [--seconds S] [--engine interpreter|block|jit] [--candump-in FILE]
//
// Boots IMAGE (a raw image of the internal flash: a kernel and an M1 application, built outside the tree), runs it for
// S seconds of simulated time (default 2), and prints what it does: the console (eSCI A) on stderr, and every frame
// the controllers put on CAN A, B and C on stdout in candump format, "(seconds) canN ID#DATA". --candump-in sends a
// candump log's frames (canN: bus N) onto the buses at their times, the first at 0.3 s.
#include <chrono>
#include <cstdio>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include "m150/board/m150.h"
#include "m150/top/candump.h"

namespace {

using namespace sim;

int usage() {
    std::fprintf(stderr,
                 "usage: m150_sim run IMAGE [--seconds S] [--engine interpreter|block|jit] [--candump-in FILE]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || std::strcmp(argv[1], "run") != 0) {
        return usage();
    }
    const std::string image = argv[2];
    double seconds = 2.0;
    std::string candump_in;
    e200::MachineConfig cfg;
    cfg.engine = e200::Engine::Jit;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--seconds" && i + 1 < argc) {
            seconds = std::atof(argv[++i]);
        } else if (arg == "--candump-in" && i + 1 < argc) {
            candump_in = argv[++i];
        } else if (arg == "--engine" && i + 1 < argc) {
            const std::string e = argv[++i];
            cfg.engine = e == "interpreter" ? e200::Engine::Interpreter : e == "block" ? e200::Engine::Block
                                                                                       : e200::Engine::Jit;
        } else {
            return usage();
        }
    }

    m150::Board board(cfg);
    try {
        board.load_flash_file(image);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "m150_sim: %s\n", e.what());
        return 1;
    }
    std::string line;
    board.soc.esci_a->on_byte = [&line](uint8_t b) {
        if (b == '\n') {
            std::fprintf(stderr, "%s\n", line.c_str());
            line.clear();
        } else if (b != '\r') {
            line += char(b);
        }
    };
    const uint64_t hz = board.m.cfg.core_hz;
    auto print = [hz](const sys::CanFrame& f, int bus, uint64_t now) {
        std::printf("%s\n", m150::format_candump(f, bus, now, hz).c_str());
    };
    m150::CanListener printers[3] = {{0, print}, {1, print}, {2, print}};
    for (int k = 0; k < 3; ++k) {
        board.soc.can_bus[size_t(k)].attach(printers[k]);
    }
    if (!board.reset()) {
        return 1;
    }
    if (!candump_in.empty()) {
        try {
            board.m.add(std::make_shared<m150::CanInjector>(board, m150::read_candump(candump_in, hz)));
        } catch (const std::exception& e) {
            std::fprintf(stderr, "m150_sim: %s\n", e.what());
            return 1;
        }
    }

    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t end = uint64_t(seconds * double(board.m.cfg.core_hz));
    const e200::Stop stop = board.m.run_until(end);
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!line.empty()) {
        std::fprintf(stderr, "%s\n", line.c_str());
    }
    std::fflush(stdout);
    std::fprintf(stderr, "m150_sim: %.3f s simulated in %.3f s; stopped (%d) at pc 0x%08x; frames: can0 %llu, can1 %llu, "
                         "can2 %llu\n",
                 double(board.m.now()) / double(board.m.cfg.core_hz), wall, int(stop.kind), board.m.state.pc,
                 static_cast<unsigned long long>(printers[0].frames), static_cast<unsigned long long>(printers[1].frames),
                 static_cast<unsigned long long>(printers[2].frames));
    return stop.kind == e200::StopKind::Limit ? 0 : 1;
}
