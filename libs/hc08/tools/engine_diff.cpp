// hc08_engine_diff --engine-diff: random CPU08 programs run on the interpreter and on another engine, compared after every
// run slice (registers, cycles, memory, and a hash of the accesses that are bus events: device reads and writes, writes
// to flash and ROM). The environment has the shapes the engines must
// agree on: RAM, flash (writes dropped), a device window (accesses take the slow path; a write to $10 raises an
// interrupt line, a write to $11 lowers the cycle limit as a device event does), code in RAM that the program may
// overwrite, interrupts with their vector, and slices of random length (so blocks meet the limit).
//
//   hc08_engine_diff --engine-diff --engine block|jit|broken [--cases N] [--seed S] [--slices K]
//
// `broken` is a deliberately wrong engine (ADD run as ADC); ctest expects it to fail.
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>

#include "hc08/jit/jit_engine.h"
#include "hc08/block_engine.h"
#include "hc08/cpu.h"
#include "hc08/memmap.h"

using namespace sim;

namespace {

struct Env {
    std::array<uint8_t, 65536> mem{};
    std::array<uint8_t, 65536> region{};
    uint64_t limit = 0;
    uint64_t hash = 1469598103934665603ull;
    uint32_t irq = 0;
    DecodeCache* cache = nullptr;

    void mix(uint64_t v) {
        hash = (hash ^ v) * 1099511628211ull;
    }

    uint8_t fetch(uint16_t a) const {
        return Region(region[a]) == Region::Device ? 0 : mem[a];
    }
    uint8_t read(uint16_t a) {
        if (Region(region[a]) == Region::Device) {
            mix(0x10000 | a);
        }
        return mem[a];
    }
    void write(uint16_t a, uint8_t v) {
        // RAM writes are compared through memory (the JIT stores plain RAM directly); the others are bus events
        if (Region(region[a]) != Region::Ram) {
            mix((uint64_t(a) << 8) | v);
        }
        switch (Region(region[a])) {
            case Region::Ram:
                mem[a] = v;
                if (cache && cache->has_code(a)) {
                    cache->invalidate(a);
                }
                break;
            case Region::Device:
                mem[a] = v;
                if (a == 0x10) {
                    irq = v & 1;
                } else if (a == 0x11) {
                    limit = std::min<uint64_t>(limit, cycles_now + (v & 7));
                }
                break;
            case Region::Unmapped:
            case Region::Flash:
            case Region::Rom:
                break;  // flash and ROM: dropped
        }
    }
    uint64_t cycle_limit() const {
        return limit;
    }
    bool irq_pending() const {
        return irq != 0;
    }
    uint16_t irq_vector() const {
        return 0xFFF2;
    }
    bool watching() const {
        return false;
    }
    bool pc_watched(uint16_t) const {
        return false;
    }
    bool irq_pin() const {
        return true;
    }
    uint8_t* jit_memory() {
        return mem.data();
    }
    const uint8_t* jit_regions() const {
        return region.data();
    }
    const uint64_t* jit_limit() const {
        return &limit;
    }
    const uint32_t* jit_irq_lines() const {
        return &irq;
    }

    uint64_t cycles_now = 0;  // the state's cycles, for the limit-lowering register
    const CpuState* state = nullptr;
};

// the limit register needs the running cycle count: read it from the state
struct EnvWithState : Env {
    void write(uint16_t a, uint8_t v) {
        cycles_now = state->cycles;
        Env::write(a, v);
    }
};

bool same(const CpuState& a, const CpuState& b) {
    return a.a == b.a && a.hx == b.hx && a.sp == b.sp && a.pc == b.pc && a.ccr == b.ccr && a.cycles == b.cycles &&
           a.irq_inhibit == b.irq_inhibit;
}

void print_state(const char* name, const CpuState& s) {
    std::fprintf(stderr, "  %-6s pc=%04X a=%02X hx=%04X sp=%04X ccr=%02X cycles=%llu\n", name, s.pc, s.a, s.hx, s.sp,
                 s.ccr, static_cast<unsigned long long>(s.cycles));
}

}  // namespace

int main(int argc, char** argv) {
    std::string engine = "block";
    int cases = 10000;
    int slices = 16;
    uint64_t seed = 1;
    bool diff = false;
    for (int k = 1; k < argc; k++) {
        const std::string a = argv[k];
        if (a == "--engine-diff") {
            diff = true;
        } else if (a == "--engine" && k + 1 < argc) {
            engine = argv[++k];
        } else if (a == "--cases" && k + 1 < argc) {
            cases = std::stoi(argv[++k]);
        } else if (a == "--slices" && k + 1 < argc) {
            slices = std::stoi(argv[++k]);
        } else if (a == "--seed" && k + 1 < argc) {
            seed = std::stoull(argv[++k]);
        }
    }
    if (!diff || (engine != "block" && engine != "jit" && engine != "broken")) {
        std::fprintf(stderr, "usage: hc08_engine_diff --engine-diff --engine block|jit|broken [--cases N] [--slices K]\n");
        return 2;
    }
    if (engine == "jit" && !jit::host_supported()) {
        std::printf("engine-diff jit: no assembler for this host, skipped\n");
        return 0;
    }

    std::mt19937_64 rng(seed);
    uint64_t cycles_total = 0;
    std::unique_ptr<jit::JitCache> jit_cache;
    if (engine == "jit") {
        jit_cache = std::make_unique<jit::JitCache>(size_t(8) << 20);
    }

    for (int c = 0; c < cases; c++) {
        auto a = std::make_unique<EnvWithState>();
        for (auto& b : a->mem) {
            b = uint8_t(rng());
        }
        // a device window, RAM, then flash; mostly-legal code: favour the common opcodes in the code areas
        for (uint32_t addr = 0; addr < 0x10000; addr++) {
            a->region[addr] = uint8_t(addr < 0x40 ? Region::Device : addr < 0x8000 ? Region::Ram : Region::Flash);
        }
        a->region[0x00FF] = uint8_t(Region::Rom);  // a dropped write inside RAM's page 0
        const uint16_t code = (rng() & 1) ? uint16_t(0x0400 + (rng() & 0x1FF)) : uint16_t(0x8000 + (rng() & 0x3FF));
        static constexpr uint8_t kCommon[] = {
            0xA6, 0xB6, 0xC6, 0xE6, 0xF6, 0xB7, 0xC7, 0xF7, 0xAB, 0xA0, 0xA1, 0xA2, 0xA9, 0xA4, 0xAA, 0xA8, 0x4C,
            0x4A, 0x5C, 0x3C, 0x3A, 0x48, 0x44, 0x46, 0x49, 0x47, 0x40, 0x43, 0x4F, 0x5F, 0x3F, 0x45, 0x55, 0x35,
            0x65, 0x75, 0xAF, 0xA7, 0x87, 0x86, 0x89, 0x88, 0x8B, 0x8A, 0x97, 0x9F, 0x95, 0x94, 0x6E, 0x4E, 0x5E,
            0x7E, 0x10, 0x11, 0x00, 0x01, 0x26, 0x27, 0x42, 0x9D, 0x98, 0x99, 0x9E, 0x26, 0x24, 0x25, 0x2A, 0x90,
            0x92, 0x4B, 0x41, 0x8C, 0x9A, 0x9B, 0x9A, 0x9B, 0x84, 0x80, 0x9A, 0xB7, 0x10};
        for (int k = 0; k < 96; k++) {
            if (rng() % 3 != 0) {
                a->mem[uint16_t(code + k)] = kCommon[rng() % sizeof(kCommon)];
            }
        }
        // the interrupt vector into the code area; the device registers start quiet
        a->mem[0xFFF2] = uint8_t(code >> 8);
        a->mem[0xFFF3] = uint8_t(code + 8);
        a->irq = 0;

        auto b = std::make_unique<EnvWithState>(*a);
        DecodeCache cache_a;
        DecodeCache cache_b;
        a->cache = &cache_a;
        b->cache = &cache_b;
        if (jit_cache) {
            jit_cache->reset();
            cache_b.on_drop = [&](unsigned page) { jit_cache->drop_page(page); };
        }

        CpuState sa;
        sa.a = uint8_t(rng());
        sa.hx = uint16_t(0x0100 + (rng() & 0x7FFF));
        sa.sp = uint16_t(0x0100 + (rng() & 0x7FF));
        sa.pc = code;
        sa.ccr = uint8_t(rng() | ccr::kOnes);
        CpuState sb = sa;
        a->state = &sa;
        b->state = &sb;

        for (int k = 0; k < slices; k++) {
            const uint64_t limit = sa.cycles + 1 + rng() % 200;
            a->limit = limit;
            b->limit = limit;
            const Stop ra = interpret(sa, *a);
            Stop rb;
            if (engine == "block") {
                rb = run_blocks(sb, *b, cache_b);
            } else if (engine == "jit") {
                rb = jit::run_jit(sb, *b, cache_b, *jit_cache);
            } else {
                rb = run_core(sb, *b, [&b](uint16_t pc) {
                    Insn insn = decode(pc, [&b](uint16_t x) { return b->fetch(x); });
                    if (insn.op == Op::Add) {
                        insn.op = Op::Adc;
                    }
                    return insn;
                });
            }

            if (ra.kind != rb.kind || ra.pc != rb.pc || !same(sa, sb) || a->hash != b->hash || a->irq != b->irq ||
                a->mem != b->mem) {
                std::fprintf(stderr, "engine-diff: case %d (seed %llu) slice %d differs (stop %d vs %d)\n", c,
                             static_cast<unsigned long long>(seed), k, int(ra.kind), int(rb.kind));
                std::fprintf(stderr, "  hash %s, irq %d vs %d, memory %s\n", a->hash == b->hash ? "same" : "differs",
                             int(a->irq), int(b->irq), a->mem == b->mem ? "same" : "differs");
                print_state("interp", sa);
                print_state(engine.c_str(), sb);
                for (uint32_t x = 0; x < 0x10000; x++) {
                    if (a->mem[x] != b->mem[x]) {
                        std::fprintf(stderr, "  memory $%04X: %02X vs %02X\n", x, a->mem[x], b->mem[x]);
                        break;
                    }
                }
                return 1;
            }
            if (ra.kind != StopKind::Limit) {
                break;  // illegal opcode, WAIT, STOP: the case ends the same way on both
            }
        }
        cycles_total += sa.cycles;
    }
    std::printf("engine-diff %s: %d cases, %llu cycles, no difference", engine.c_str(), cases,
                static_cast<unsigned long long>(cycles_total));
    if (jit_cache) {
        std::printf(" (%llu blocks translated, %llu run, %llu instructions outside blocks, %llu dropped)",
                    static_cast<unsigned long long>(jit_cache->stats.translated), static_cast<unsigned long long>(jit_cache->stats.blocks_run),
                    static_cast<unsigned long long>(jit_cache->stats.interpreted), static_cast<unsigned long long>(jit_cache->stats.dropped));
    }
    std::printf("\n");
    return 0;
}
