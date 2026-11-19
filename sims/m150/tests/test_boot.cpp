// The M150 boots an image built outside the tree and runs it: the image's application transmits on CAN A, and what it
// transmits follows what it receives. Skipped without M150_IMAGE (the research tree's emu/m150_mvp builds an image:
// its kernel with the test application, which sends ltc_lambda(0) * 1000 on 0x123 every 10 ms and reads LTC #1 on
// 0x460). Every engine must give the same frames at the same times.
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "mctest/test.h"
#include "m150/top/candump.h"

using namespace sim;

namespace {

const char* image() {
    const char* path = std::getenv("M150_IMAGE");
    return (path && *path) ? path : nullptr;
}

struct Run {
    std::vector<std::string> frames;  // on CAN A, from the board
    std::string console;
};

// LTC #1 reporting lambda 0.850, every 20 ms from 0.3 s
std::string ltc_log() {
    std::string out;
    for (int k = 0; k < 50; ++k) {
        char line[96];
        std::snprintf(line, sizeof line, "(%.6f) can0 460#00035200102D0032\n", k * 0.02);
        out += line;
        std::snprintf(line, sizeof line, "(%.6f) can0 460#01070564002000FA\n", k * 0.02);
        out += line;
    }
    return out;
}

Run run(e200::Engine engine, double seconds, bool with_ltc) {
    e200::MachineConfig cfg;
    cfg.engine = engine;
    m150::Board board(cfg);
    board.load_flash_file(image());
    Run r;
    board.soc.esci_a->on_byte = [&r](uint8_t b) { r.console += char(b); };
    const uint64_t hz = board.m.cfg.core_hz;
    m150::CanListener listener(0, [&r, hz](const sys::CanFrame& f, int bus, uint64_t now) {
        if (f.id != 0x460) {
            r.frames.push_back(m150::format_candump(f, bus, now, hz));
        }
    });
    board.soc.can_bus[0].attach(listener);
    CHECK(board.reset());
    if (with_ltc) {
        board.m.add(std::make_shared<m150::CanInjector>(board, m150::parse_candump(ltc_log(), hz)));
    }
    const e200::Stop stop = board.m.run_until(uint64_t(seconds * double(hz)));
    CHECK(stop.kind == e200::StopKind::Limit);
    return r;
}

size_t count_with(const std::vector<std::string>& frames, const std::string& tail) {
    size_t n = 0;
    for (const std::string& f : frames) {
        n += f.size() >= tail.size() && f.compare(f.size() - tail.size(), tail.size(), tail) == 0;
    }
    return n;
}

}  // namespace

TEST(m150_boots_the_image_and_transmits) {
    if (!image()) {
        mctest::skip("M150_IMAGE not set");
        return;
    }
    const Run r = run(e200::Engine::Jit, 1.0, false);
    CHECK(r.console.find("application: entry") != std::string::npos);
    CHECK(r.console.find("exception") == std::string::npos);
    // 0x123 at 100 Hz from the application's first trigger: 100 frames in the first second
    CHECK_EQ(r.frames.size(), size_t(100));
    CHECK_EQ(count_with(r.frames, "can0 123#0000000000000000"), size_t(100));
}

TEST(m150_application_answers_what_it_receives) {
    if (!image()) {
        mctest::skip("M150_IMAGE not set");
        return;
    }
    const Run r = run(e200::Engine::Jit, 1.0, true);
    // lambda 0.850 from 0.3 s: 850 = 0x0352 in the first 16 bits from the next 10 ms frame on
    CHECK_EQ(count_with(r.frames, "can0 123#0352000000000000"), size_t(69));
    CHECK_EQ(count_with(r.frames, "can0 123#0000000000000000"), size_t(31));
}

TEST(m150_every_engine_gives_the_same_frames) {
    if (!image()) {
        mctest::skip("M150_IMAGE not set");
        return;
    }
    const Run jit = run(e200::Engine::Jit, 0.6, true);
    const Run block = run(e200::Engine::Block, 0.6, true);
    const Run interpreter = run(e200::Engine::Interpreter, 0.6, true);
    CHECK(jit.frames == interpreter.frames);
    CHECK(block.frames == interpreter.frames);
    CHECK(jit.console == interpreter.console);
}
