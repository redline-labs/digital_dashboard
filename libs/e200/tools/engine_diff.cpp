// e200_engine_diff: random programs on the interpreter and on another engine, compared at random points.
//
//   e200_engine_diff --engine block|jit|broken [--cases N] [--seed S]
//
// Each case assembles a random program (integer, logical, rotate, compare, CR, branch, load/store including the update,
// multiple, byte-reversed and reserved forms, SPE, LR/CTR/XER moves, sc with a handler) into RAM, starts both machines
// from the same random registers, and runs them to the same sequence of random times (1..40 cycles apart, so blocks
// are cut by the limit as often as they run whole), comparing every register, the cycle count and the scratch memory
// after each. `broken` is the JIT with one register bit flipped part-way: the checker must catch it (the test expects
// a failure), or a checker that cannot fail would prove nothing.
#include <chrono>
#include <ctime>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>

#include "e200/assembler.h"
#include "e200/machine.h"

using namespace sim::e200;

namespace {

constexpr uint32_t kCode = 0x40000000;
constexpr uint32_t kData = 0x40010000;  // r31 points here; loads and stores stay within 4 KB of it (and across)
constexpr uint32_t kHandlers = 0x40020000;

std::unique_ptr<Machine> make_machine(Engine engine) {
    MachineConfig cfg;
    cfg.engine = engine;
    auto m = std::make_unique<Machine>(cfg);
    m->map_ram(0x40000000, 0x40000);
    m->state.tlb1[1] = TlbEntry{0x80000500, 0x40000000, 0x4000003F};
    m->state.ivpr = kHandlers;
    for (size_t v = 0; v < 16; ++v) {
        m->state.ivor[v] = uint32_t(v * 0x100);
    }
    m->translation_changed();
    return m;
}

struct Generator {
    std::mt19937 rng;
    explicit Generator(uint32_t seed) : rng(seed) {
    }
    uint32_t u(uint32_t n) {
        return uint32_t(rng() % n);
    }
    int reg() {
        return int(u(31));  // r0..r30: r31 is the data base
    }
    bool coin() {
        return u(2) == 0;
    }

    void instruction(Asm& a, std::vector<Asm::Label>& forward) {
        const int d = reg(), x = reg(), y = reg();
        const int16_t simm = int16_t(int32_t(u(65536)) - 32768);
        const uint16_t uimm = uint16_t(u(65536));
        const bool rc = coin();
        switch (u(44)) {
            case 0: a.addi(d, x, simm); break;
            case 1: a.addis(d, x, simm); break;
            case 2: a.addic(d, x, simm); break;
            case 3: a.addic_(d, x, simm); break;
            case 4: a.subfic(d, x, simm); break;
            case 5: a.mulli(d, x, simm); break;
            case 6: a.add(d, x, y, rc, coin()); break;
            case 7: a.addc(d, x, y, rc, coin()); break;
            case 8: a.adde(d, x, y, rc, coin()); break;
            case 9: a.subf(d, x, y, rc, coin()); break;
            case 10: a.subfe(d, x, y, rc, coin()); break;
            case 11: a.neg(d, x, rc, coin()); break;
            case 12: a.mullw(d, x, y, rc, coin()); break;
            case 13: coin() ? a.mulhw(d, x, y) : a.mulhwu(d, x, y); break;
            case 14: coin() ? a.divw(d, x, y, rc, coin()) : a.divwu(d, x, y, rc, coin()); break;
            case 15:
                switch (u(8)) {
                    case 0: a.and_(d, x, y, rc); break;
                    case 1: a.andc(d, x, y, rc); break;
                    case 2: a.or_(d, x, y, rc); break;
                    case 3: a.orc(d, x, y, rc); break;
                    case 4: a.xor_(d, x, y, rc); break;
                    case 5: a.nand(d, x, y, rc); break;
                    case 6: a.nor(d, x, y, rc); break;
                    default: a.eqv(d, x, y, rc); break;
                }
                break;
            case 16:
                switch (u(6)) {
                    case 0: a.ori(d, x, uimm); break;
                    case 1: a.oris(d, x, uimm); break;
                    case 2: a.xori(d, x, uimm); break;
                    case 3: a.andi_(d, x, uimm); break;
                    case 4: a.andis_(d, x, uimm); break;
                    default: a.extsb(d, x, rc); break;
                }
                break;
            case 17: coin() ? a.extsh(d, x, rc) : a.cntlzw(d, x, rc); break;
            case 18: a.rlwinm(d, x, int(u(32)), int(u(32)), int(u(32)), rc); break;
            case 19: a.rlwnm(d, x, y, int(u(32)), int(u(32)), rc); break;
            case 20: a.rlwimi(d, x, int(u(32)), int(u(32)), int(u(32)), rc); break;
            case 21:
                switch (u(4)) {
                    case 0: a.slw(d, x, y, rc); break;
                    case 1: a.srw(d, x, y, rc); break;
                    case 2: a.sraw(d, x, y, rc); break;
                    default: a.srawi(d, x, int(u(32)), rc); break;
                }
                break;
            case 22: coin() ? a.cmpw(int(u(8)), x, y) : a.cmplw(int(u(8)), x, y); break;
            case 23: coin() ? a.cmpwi(int(u(8)), x, simm) : a.cmplwi(int(u(8)), x, uimm); break;
            case 24: a.isel(d, x, y, int(u(32))); break;
            case 25: coin() ? a.mtcrf(int(u(256)), x) : a.mfcr(d); break;
            case 26: coin() ? a.crxor(int(u(32)), int(u(32)), int(u(32))) : a.cror(int(u(32)), int(u(32)), int(u(32)));
                break;
            case 27: {
                const int spr = u(3) == 0 ? 1 : coin() ? 8 : 9;
                coin() ? a.mtspr(spr, x) : a.mfspr(d, spr);
                break;
            }
            // memory, relative to r31 (data) through a scratch base r30 = r31 + small offset
            case 28: case 29: case 30: {
                const int16_t off = int16_t(int32_t(u(4100)) - 4);
                switch (u(10)) {
                    case 0: a.lwz(d, off, 31); break;
                    case 1: a.lbz(d, off, 31); break;
                    case 2: a.lhz(d, off, 31); break;
                    case 3: a.lha(d, off, 31); break;
                    case 4: a.stw(x, off, 31); break;
                    case 5: a.stb(x, off, 31); break;
                    case 6: a.sth(x, off, 31); break;
                    case 7:
                        a.addi(30, 31, int16_t(u(4000)));
                        coin() ? a.lwzu(d == 30 ? 29 : d, int16_t(u(64)), 30) : a.stwu(x, int16_t(u(64)), 30);
                        break;
                    case 8:
                        a.li(30, int16_t(u(4096)));
                        coin() ? a.lwzx(d == 30 ? 29 : d, 31, 30) : a.stwx(x, 31, 30);
                        break;
                    default:
                        a.li(30, int16_t(u(4096)));
                        coin() ? a.lwbrx(d == 30 ? 29 : d, 31, 30) : a.stwbrx(x, 31, 30);
                        break;
                }
                break;
            }
            case 31: {
                const int first = 24 + int(u(7));
                a.addi(30, 31, int16_t(u(1000) * 4));
                coin() ? a.stmw(first, 0, 30) : a.lmw(first == 30 ? 29 : first, 0, 30);
                break;
            }
            case 32:
                a.addi(30, 31, int16_t(u(64) * 4));
                coin() ? a.lwarx(d == 30 ? 29 : d, 0, 30) : a.stwcx_(x, 0, 30);
                break;
            case 33: a.efsadd(d, x, y); break;
            case 34: a.efsmul(d, x, y); break;
            case 35: coin() ? a.efsdiv(d, x, y) : a.efssub(d, x, y); break;
            case 36: coin() ? a.efscfsi(d, x) : a.efsctsiz(d, x); break;
            case 37: a.efscmpgt(int(u(8)), x, y); break;
            // a forward branch over a few instructions (bound later)
            case 38: case 39: {
                forward.push_back(a.label());
                switch (u(4)) {
                    case 0: a.b(forward.back()); break;
                    case 1: a.beq(forward.back(), int(u(8))); break;
                    case 2: a.blt(forward.back(), int(u(8))); break;
                    default: a.bgt(forward.back(), int(u(8))); break;
                }
                break;
            }
            case 40: a.sc(); break;
            case 41: a.mtcrf(0xFF, x); break;
            case 42: a.nop(); break;
            default: a.dcbz(31, 30); break;
        }
    }

    // A program: random instructions, forward branches bound a few instructions on, and a jump back to the start.
    Asm program(size_t length) {
        Asm a(kCode);
        auto start = a.label();
        a.bind(start);
        std::vector<Asm::Label> forward;
        std::vector<std::pair<Asm::Label, size_t>> pending;
        for (size_t i = 0; i < length; ++i) {
            const size_t before = forward.size();
            instruction(a, forward);
            if (forward.size() > before) {
                pending.push_back({forward.back(), i + 1 + u(6)});
            }
            for (auto it = pending.begin(); it != pending.end();) {
                if (it->second <= i) {
                    a.bind(it->first);
                    it = pending.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& p : pending) {
            a.bind(p.first);
        }
        a.b(start);
        return a;
    }
};

void load(Machine& m, const Asm& a) {
    const auto bytes = a.bytes();
    m.load_image(a.origin(), bytes.data(), bytes.size());
}

// The handlers: sc and every other interrupt return past (sc) or over (the rest) the instruction.
void install_handlers(Machine& m) {
    for (uint32_t v = 0; v < 16; ++v) {
        Asm h(kHandlers + v * 0x100);
        if (v != 8) {
            h.mfspr(29, spr::kSrr0);
            h.addi(29, 29, 4);
            h.mtspr(spr::kSrr0, 29);
        }
        h.addi(28, 28, 1);
        h.rfi();
        load(m, h);
    }
}

bool compare(const Machine& a, const Machine& b, std::string* why) {
    const CpuState& x = a.state;
    const CpuState& y = b.state;
    char buf[160];
    for (size_t r = 0; r < 32; ++r) {
        if (x.gpr[r] != y.gpr[r]) {
            std::snprintf(buf, sizeof buf, "r%zu %08x != %08x", r, x.gpr[r], y.gpr[r]);
            *why = buf;
            return false;
        }
    }
    const struct {
        const char* name;
        uint64_t p, q;
    } fields[] = {
        {"pc", x.pc, y.pc},       {"cr", x.cr, y.cr},         {"xer", x.xer, y.xer},
        {"lr", x.lr, y.lr},       {"ctr", x.ctr, y.ctr},      {"msr", x.msr, y.msr},
        {"srr0", x.srr0, y.srr0}, {"cycles", x.cycles, y.cycles}, {"spefscr", x.spefscr, y.spefscr},
        {"reserved", x.reserved, y.reserved},
    };
    for (const auto& f : fields) {
        if (f.p != f.q) {
            std::snprintf(buf, sizeof buf, "%s %llx != %llx", f.name, static_cast<unsigned long long>(f.p), static_cast<unsigned long long>(f.q));
            *why = buf;
            return false;
        }
    }
    auto& ma = const_cast<Machine&>(a);
    auto& mb = const_cast<Machine&>(b);
    if (std::memcmp(ma.host(kData), mb.host(kData), 0x2000) != 0) {
        *why = "data memory differs";
        return false;
    }
    return true;
}

}  // namespace

namespace {

// --bench [ENGINE [WORKLOAD [SCALE]]]: workloads like compiled firmware, on each engine (or one), in 100 us quanta:
//   checksum  a load/ALU/store loop with a compare and a counted branch
//   calls     a function call per iteration: stack frame, LR save and restore, a leaf call, return
//   float     SPE single-precision multiply-add and compare
//   copy      a byte copy (lbzu/stbu) and a word clear
Asm bench_checksum() {
    Asm a(kCode);
    auto outer = a.label();
    auto inner = a.label();
    a.bind(outer);
    a.li32(3, kData);
    a.li(4, 256);
    a.mtctr(4);
    a.bind(inner);
    a.lwz(5, 0, 3);
    a.add(6, 6, 5);
    a.rlwinm(7, 6, 3, 0, 31);
    a.xor_(6, 6, 7);
    a.stw(6, 4, 3);
    a.addi(3, 3, 8);
    a.cmplw(0, 3, 4);
    a.bdnz(inner);
    a.b(outer);
    return a;
}

Asm bench_calls() {
    Asm a(kCode);
    auto loop = a.label();
    auto fn = a.label();
    auto leaf = a.label();
    a.li32(1, kData + 0x1F00);  // the stack
    a.li(20, 0);
    a.bind(loop);
    a.mr(3, 20);
    a.bl(fn);
    a.add(21, 21, 3);
    a.addi(20, 20, 1);
    a.cmpwi(0, 20, 1000);
    a.blt(loop);
    a.li(20, 0);
    a.b(loop);
    // fn(x): a frame, LR saved, two callee-saved registers, a leaf call
    a.bind(fn);
    a.stwu(1, -32, 1);
    a.mflr(0);
    a.stw(0, 36, 1);
    a.stw(30, 24, 1);
    a.stw(31, 28, 1);
    a.mr(31, 3);
    a.addi(3, 3, 7);
    a.bl(leaf);
    a.add(3, 3, 31);
    a.lwz(0, 36, 1);
    a.lwz(30, 24, 1);
    a.lwz(31, 28, 1);
    a.mtlr(0);
    a.addi(1, 1, 32);
    a.blr();
    a.bind(leaf);
    a.slwi(4, 3, 2);
    a.cmpwi(0, 4, 100);
    a.isel(3, 4, 3, 1);
    a.blr();
    return a;
}

Asm bench_float() {
    Asm a(kCode);
    auto loop = a.label();
    auto skip = a.label();
    a.li32(3, 0x3F800000);   // 1.0
    a.li32(4, 0x3F7FBE77);   // 0.999
    a.li32(5, 0);
    a.li32(9, kData);
    a.bind(loop);
    a.lwz(6, 0, 9);
    a.efsmul(3, 3, 4);
    a.efsadd(5, 5, 3);
    a.efscmpgt(1, 5, 4);
    a.ble(skip, 1);
    a.efsmul(5, 5, 4);
    a.bind(skip);
    a.stw(5, 4, 9);
    a.b(loop);
    return a;
}

Asm bench_copy() {
    Asm a(kCode);
    auto outer = a.label();
    auto copy = a.label();
    auto clear = a.label();
    a.bind(outer);
    a.li32(3, kData - 1);
    a.li32(4, kData + 0x800 - 1);
    a.li(5, 1024);
    a.mtctr(5);
    a.bind(copy);
    a.lbzu(6, 1, 3);
    a.stbu(6, 1, 4);
    a.bdnz(copy);
    a.li32(3, kData + 0x1000);
    a.li(5, 256);
    a.mtctr(5);
    a.li(6, 0);
    a.bind(clear);
    a.stw(6, 0, 3);
    a.addi(3, 3, 4);
    a.bdnz(clear);
    a.b(outer);
    return a;
}

int bench(const std::string& only, const std::string& workload, uint64_t scale) {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);  // a performance core where there is one
#endif
    const struct {
        const char* name;
        Asm (*program)();
    } workloads[] = {{"checksum", bench_checksum}, {"calls", bench_calls}, {"float", bench_float}, {"copy", bench_copy}};
    for (const auto& w : workloads) {
        if (!workload.empty() && workload != w.name) {
            continue;
        }
        for (Engine e : {Engine::Interpreter, Engine::Block, Engine::Jit}) {
            const char* name = e == Engine::Interpreter ? "interpreter" : e == Engine::Block ? "block" : "jit";
            if (!only.empty() && only != name) {
                continue;
            }
            const uint64_t cycles = e == Engine::Interpreter ? 20'000'000 : e == Engine::Block ? 30'000'000
                                                                                                : 100'000'000 * scale;
            // the best of five runs of thread CPU time: other work on the machine only ever slows a run down
            double best = 1e30;
            for (int rep = 0; rep < 5; ++rep) {
                auto m = make_machine(e);
                load(*m, w.program());
                m->state.pc = kCode;
                timespec t0{}, t1{};
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
                m->run_until(cycles);
                clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t1);
                best = std::min(best, double(t1.tv_sec - t0.tv_sec) + double(t1.tv_nsec - t0.tv_nsec) * 1e-9);
            }
            const double s = best;
            std::printf("%-9s %-12s %7.1f M instructions/s (%4.1fx real time at 125 MHz)\n", w.name, name,
                        double(cycles) / s / 1e6, double(cycles) / s / 125e6);
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--bench") {
        return bench(argc >= 3 ? argv[2] : "", argc >= 4 ? argv[3] : "", argc >= 5 ? std::strtoull(argv[4], nullptr, 0) : 1);
    }
    std::string engine_name = "jit";
    int cases = 2000;
    uint32_t seed = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--engine" && i + 1 < argc) {
            engine_name = argv[++i];
        } else if (arg == "--cases" && i + 1 < argc) {
            cases = std::atoi(argv[++i]);
        } else if (arg == "--seed" && i + 1 < argc) {
            seed = uint32_t(std::strtoul(argv[++i], nullptr, 0));
        } else if (arg == "--engine-diff") {
            // accepted for symmetry with hc08_engine_diff
        } else {
            std::fprintf(stderr, "usage: e200_engine_diff --engine block|jit|broken [--cases N] [--seed S]\n");
            return 2;
        }
    }
    const bool broken = engine_name == "broken";
    const Engine engine = engine_name == "block" ? Engine::Block : Engine::Jit;
    if (engine_name != "block" && engine_name != "jit" && !broken) {
        std::fprintf(stderr, "unknown engine %s\n", engine_name.c_str());
        return 2;
    }

    uint64_t compared = 0;
    uint64_t blocks = 0, interpreted = 0;
    for (int c = 0; c < cases; ++c) {
        Generator g(seed * 7919u + uint32_t(c));
        const Asm program = g.program(20 + g.u(120));
        auto ref = make_machine(Engine::Interpreter);
        auto test = make_machine(engine);
        for (Machine* m : {ref.get(), test.get()}) {
            load(*m, program);
            install_handlers(*m);
        }
        // the same random registers and data on both
        Generator init(seed * 104729u + uint32_t(c));
        for (size_t r = 0; r < 31; ++r) {
            const uint32_t v = init.u(4) == 0 ? init.u(64) : uint32_t(init.rng());
            ref->state.gpr[r] = test->state.gpr[r] = v;
        }
        ref->state.gpr[31] = test->state.gpr[31] = kData;
        ref->state.cr = test->state.cr = uint32_t(init.rng());
        ref->state.xer = test->state.xer = uint32_t(init.rng()) & 0xE0000000;
        ref->state.ctr = test->state.ctr = init.u(50);
        for (uint32_t off = 0; off < 0x2000; off += 4) {
            const uint32_t v = uint32_t(init.rng());
            const uint8_t bytes[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
            ref->load_image(kData + off, bytes, 4);
            test->load_image(kData + off, bytes, 4);
        }
        ref->state.pc = test->state.pc = kCode;

        uint64_t t = 0;
        const uint64_t end = 3000;
        int step = 0;
        while (t < end) {
            t += 1 + g.u(40);
            const Stop sa = ref->run_until(t);
            const Stop sb = test->run_until(t);
            if (broken && step == 20) {
                test->state.gpr[g.u(31)] ^= 0x10;
            }
            ++step;
            std::string why;
            if (sa.kind != sb.kind) {
                why = "stop kinds differ";
            }
            if (!why.empty() || !compare(*ref, *test, &why)) {
                std::fprintf(stderr, "case %d (seed %u) differs at t=%llu: %s\n", c, seed,
                             static_cast<unsigned long long>(t), why.c_str());
                return 1;
            }
            ++compared;
            if (sa.kind == StopKind::Checkstop) {
                break;
            }
        }
        if (const auto* js = test->jit_stats()) {
            blocks += js->blocks_run;
            interpreted += js->interpreted;
        }
    }
    if (blocks + interpreted) {
        std::printf("jit: %llu blocks run, %llu instructions interpreted\n", static_cast<unsigned long long>(blocks),
                    static_cast<unsigned long long>(interpreted));
    }
    std::printf("%d cases, %llu comparisons: %s and the interpreter agree\n", cases, static_cast<unsigned long long>(compared),
                engine_name.c_str());
    return 0;
}
