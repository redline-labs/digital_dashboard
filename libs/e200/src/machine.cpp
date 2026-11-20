#include "e200/machine.h"

#include <algorithm>

#include <spdlog/spdlog.h>

namespace sim::e200 {

namespace {

constexpr uint32_t kPage = 0x1000;

}  // namespace

Machine::Machine(MachineConfig config) : cfg(config) {
    reset_core(state);
    tlb_seen_ = state.tlb1;
}

Machine::~Machine() = default;

// ------------------------------------------------------------------------------------------------ the bus

Machine::Page* Machine::page(uint32_t pa) {
    PageBlock* b = blocks_[pa >> 20].get();
    return b ? &b->pages[(pa >> 12) & 0xFF] : nullptr;
}

Machine::Page& Machine::page_for_map(uint32_t pa) {
    std::unique_ptr<PageBlock>& b = blocks_[pa >> 20];
    if (!b) {
        b = std::make_unique<PageBlock>();
    }
    return b->pages[(pa >> 12) & 0xFF];
}

uint8_t* Machine::map_ram(uint32_t base, uint32_t size) {
    storage_.push_back(std::make_unique<uint8_t[]>(size));
    storage_sizes_.push_back(size);
    uint8_t* mem = storage_.back().get();
    std::memset(mem, 0, size);
    for (uint32_t off = 0; off < size; off += kPage) {
        Page& p = page_for_map(base + off);
        p = Page{mem + off, mem + off, nullptr, 0};
    }
    flush_soft_tlbs();
    return mem;
}

uint8_t* Machine::map_rom(uint32_t base, uint32_t size, BusDevice* controller) {
    storage_.push_back(std::make_unique<uint8_t[]>(size));
    storage_sizes_.push_back(size);
    uint8_t* mem = storage_.back().get();
    std::memset(mem, 0xFF, size);  // erased flash
    for (uint32_t off = 0; off < size; off += kPage) {
        Page& p = page_for_map(base + off);
        p = Page{mem + off, nullptr, controller, off};
    }
    if (controller) {
        controller->m = this;
    }
    flush_soft_tlbs();
    return mem;
}

void Machine::map(uint32_t base, uint32_t size, BusDevice& device, uint32_t offset) {
    for (uint32_t off = 0; off < size; off += kPage) {
        Page& p = page_for_map(base + off);
        p = Page{nullptr, nullptr, &device, offset + off};
    }
    device.m = this;
    flush_soft_tlbs();
}

uint8_t* Machine::host(uint32_t pa) {
    Page* p = page(pa);
    return (p && p->read) ? p->read + (pa & 0xFFF) : nullptr;
}

bool Machine::load_image(uint32_t pa, const uint8_t* bytes, size_t size) {
    for (size_t i = 0; i < size;) {
        uint8_t* h = host(uint32_t(pa + i));
        if (!h) {
            return false;
        }
        const size_t chunk = std::min<size_t>(size - i, kPage - ((pa + i) & 0xFFF));
        std::memcpy(h, bytes + i, chunk);
        i += chunk;
    }
    memory_changed(pa, uint32_t(size));
    return true;
}

void Machine::memory_changed(uint32_t pa, uint32_t size) {
    if (size == 0) {
        return;
    }
    for (uint64_t at = pa & ~0xFFFu; at < uint64_t(pa) + size; at += kPage) {
        code_written(uint32_t(at));
    }
}

// ------------------------------------------------------------------------------------------- devices

void Machine::add(std::shared_ptr<sys::Timed> device) {
    if (auto* bus = dynamic_cast<BusDevice*>(device.get())) {
        bus->m = this;
    }
    schedule_.add(*device);
    devices_.push_back(std::move(device));
}

sys::Timed* Machine::find(std::string_view name) const {
    for (const auto& d : devices_) {
        if (d->name() == name) {
            return d.get();
        }
    }
    return nullptr;
}

uint32_t Machine::device_read(Page& p, uint32_t pa, unsigned size) {
    BusDevice& d = *p.device;
    d.sync(state.cycles);
    const uint32_t v = d.read(p.device_offset + (pa & 0xFFF), size);
    after_device_access(d);
    return v;
}

void Machine::device_write(Page& p, uint32_t pa, unsigned size, uint32_t value) {
    BusDevice& d = *p.device;
    d.sync(state.cycles);
    d.write(p.device_offset + (pa & 0xFFF), size, value);
    after_device_access(d);
}

void Machine::after_device_access(BusDevice& device) {
    reschedule(device.next_event());
}

// -------------------------------------------------------------------------------------- translation

void Machine::select_soft_tlb() {
    const uint32_t msr = state.msr;
    const size_t index = ((msr & kMsrPr) ? 4u : 0u) | ((msr & kMsrIs) ? 2u : 0u) | ((msr & kMsrDs) ? 1u : 0u);
    soft_ = &soft_tlbs_[index];
}

void Machine::flush_soft_tlbs() {
    for (SoftTlb& t : soft_tlbs_) {
        t = SoftTlb{};
    }
}

void Machine::translation_changed() {
    // The MSR's mode selects a set; only a change to the TLB or the PID makes the sets stale.
    if (state.pid0 != pid_seen_ || std::memcmp(state.tlb1.data(), tlb_seen_.data(), sizeof(tlb_seen_)) != 0) {
        tlb_seen_ = state.tlb1;
        pid_seen_ = state.pid0;
        flush_soft_tlbs();
    }
    select_soft_tlb();
}

Fault Machine::translate(uint32_t ea, Access access, uint32_t* pa, SoftTlbEntry* fill) {
    int entry = -1;
    const Fault f = mmu_translate(state, ea, access, pa, &entry);
    if (f != Fault::None) {
        return f;
    }
    if (fill && state.tlb1[size_t(entry)].size() >= kPage &&
        !(access != Access::Execute && !data_watches_.empty() && watched_page(ea))) {
        Page* p = page(*pa);
        uint8_t* h = nullptr;
        if (p) {
            h = access == Access::Write ? p->write : p->read;
        }
        const bool code = code_pages_.test(*pa >> 12);
        if (h && !(access == Access::Write && code)) {
            fill->tag = ea & ~0xFFFu;
            fill->host = h;
            fill->phys = *pa & ~0xFFFu;
        }
    }
    return Fault::None;
}

Fault Machine::fetch_slow(uint32_t ea, uint32_t* word) {
    uint32_t pa = 0;
    const Fault f = translate(ea, Access::Execute, &pa, &soft_->exec[(ea >> 12) & kSoftMask]);
    if (f != Fault::None) {
        return f;
    }
    Page* p = page(pa);
    if (!p || !p->read) {
        return Fault::Bus;  // no code runs from a device
    }
    *word = load_be(p->read + (pa & 0xFFC), 4);
    return Fault::None;
}

Fault Machine::load_slow(uint32_t ea, unsigned size, uint32_t* value) {
    if ((ea & 0xFFF) > kPage - size) {
        // across a page: byte by byte, each translated
        uint32_t v = 0;
        for (unsigned i = 0; i < size; ++i) {
            uint32_t b = 0;
            if (const Fault f = load_slow(ea + i, 1, &b); f != Fault::None) {
                return f;
            }
            v = (v << 8) | b;
        }
        *value = v;
        return Fault::None;
    }
    uint32_t pa = 0;
    const Fault f = translate(ea, Access::Read, &pa, &soft_->read[(ea >> 12) & kSoftMask]);
    if (f != Fault::None) {
        return f;
    }
    Page* p = page(pa);
    if (p && p->read) {
        *value = load_be(p->read + (pa & 0xFFF), size);
    } else if (p && p->device) {
        *value = device_read(*p, pa, size);
    } else {
        return Fault::Bus;
    }
    if (!data_watches_.empty()) {
        check_data_watch(ea, size, *value, false);
    }
    return Fault::None;
}

Fault Machine::store_slow(uint32_t ea, unsigned size, uint32_t value) {
    if ((ea & 0xFFF) > kPage - size) {
        // translate every byte before writing any, so a fault leaves memory unchanged
        for (unsigned i = 0; i < size; ++i) {
            uint32_t pa = 0;
            if (const Fault f = translate(ea + i, Access::Write, &pa, nullptr); f != Fault::None) {
                return f;
            }
        }
        for (unsigned i = 0; i < size; ++i) {
            if (const Fault f = store_slow(ea + i, 1, value >> (8 * (size - 1 - i))); f != Fault::None) {
                return f;
            }
        }
        return Fault::None;
    }
    uint32_t pa = 0;
    const Fault f = translate(ea, Access::Write, &pa, &soft_->write[(ea >> 12) & kSoftMask]);
    if (f != Fault::None) {
        return f;
    }
    if (state.reserved && (state.reservation & ~3u) == (ea & ~3u)) {
        state.reserved = false;
    }
    if (!data_watches_.empty()) {
        check_data_watch(ea, size, value, true);
    }
    Page* p = page(pa);
    if (p && p->write) {
        if (code_pages_.test(pa >> 12)) {
            code_written(pa);
        }
        store_be(p->write + (pa & 0xFFF), size, value);
        return Fault::None;
    }
    if (p && p->device) {
        device_write(*p, pa, size, value);
        return Fault::None;
    }
    return Fault::Bus;
}

// ------------------------------------------------------------------------------------- decoded code

void Machine::code_written(uint32_t pa) {
    const uint32_t pg = pa >> 12;
    if (code_pages_.test(pg)) {
        drop_code_page(pg);
    }
}

void Machine::drop_code_page(uint32_t pg) {
    code_pages_.reset(pg);
    code_.erase(pg);
    if (jit_) {
        jit_->drop_page(pg);
        jit_dropped_ = true;
    }
    if (last_code_page_ == pg) {
        last_code_page_ = ~0u;
        last_code_ = nullptr;
    }
    stats_.invalidated++;
}

Fault Machine::exec_physical(uint32_t pc, uint32_t* pa) {
    // through the exec soft TLB, else the MMU (which fills it)
    const SoftTlbEntry& e = soft_->exec[(pc >> 12) & kSoftMask];
    if (e.tag == (pc & ~0xFFFu)) {
        *pa = e.phys | (pc & 0xFFF);
        return Fault::None;
    }
    return translate(pc, Access::Execute, pa, &soft_->exec[(pc >> 12) & kSoftMask]);
}

Fault Machine::next_insn(uint32_t pc, Insn* in) {
    uint32_t pa = 0;
    if (const Fault f = exec_physical(pc, &pa); f != Fault::None) {
        return f;
    }
    const uint32_t pg = pa >> 12;
    CodePage* cp = last_code_;
    if (pg != last_code_page_) {
        std::unique_ptr<CodePage>& slot = code_[pg];
        if (!slot) {
            Page* p = page(pa);
            if (!p || !p->read) {
                code_.erase(pg);
                return Fault::Bus;
            }
            slot = std::make_unique<CodePage>();
            code_pages_.set(pg);
            // writes to this page must now go the slow way
            for (SoftTlb& t : soft_tlbs_) {
                for (SoftTlbEntry& w : t.write) {
                    if (w.tag != kInvalidTag && w.host == p->write) {
                        w = SoftTlbEntry{};
                    }
                }
            }
        }
        cp = slot.get();
        last_code_page_ = pg;
        last_code_ = cp;
    }
    const size_t index = (pa & 0xFFF) >> 2;
    if (!cp->valid.test(index)) {
        uint32_t word = 0;
        if (const Fault f = fetch(pc, &word); f != Fault::None) {
            return f;
        }
        cp->insn[index] = decode(word);
        cp->valid.set(index);
        stats_.decoded++;
    }
    *in = cp->insn[index];
    return Fault::None;
}

// --------------------------------------------------------------------------------------- interrupts

void Machine::set_external_input(bool on) {
    external_ = on;
    if (on) {
        limit_cycles_ = std::min(limit_cycles_, state.cycles);  // checked before the next instruction
    }
}

void Machine::set_critical_input(bool on) {
    critical_ = on;
    if (on) {
        limit_cycles_ = std::min(limit_cycles_, state.cycles);
    }
}

Stop Machine::on_interrupt(Vector v, uint32_t pc) {
    stats_.interrupts++;
    const unsigned n = unsigned(v);
    if (n < 64 && (caught_vectors_ >> n) & 1) {
        caught_ = true;
        return Stop{StopKind::Break, pc, n};
    }
    return Stop{};
}

void Machine::catch_vector(Vector v, bool on) {
    const uint64_t bit = uint64_t(1) << unsigned(v);
    caught_vectors_ = on ? (caught_vectors_ | bit) : (caught_vectors_ & ~bit);
}

void Machine::watch_pc(uint32_t pc, bool on) {
    auto it = std::find(pc_watches_.begin(), pc_watches_.end(), pc);
    if (on && it == pc_watches_.end()) {
        pc_watches_.push_back(pc);
    } else if (!on && it != pc_watches_.end()) {
        pc_watches_.erase(it);
    }
}

void Machine::clear_pc_watches() {
    pc_watches_.clear();
}

bool Machine::pc_watched(uint32_t pc) const {
    return std::find(pc_watches_.begin(), pc_watches_.end(), pc) != pc_watches_.end();
}

// ---------------------------------------------------------------------------------------- debugger

bool Machine::peek(uint32_t ea, unsigned size, uint32_t* value) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; ++i) {
        uint32_t pa = 0;
        if (mmu_translate(state, ea + i, Access::Read, &pa) == Fault::TlbMiss) {
            return false;
        }
        Page* p = page(pa);
        uint32_t b = 0;
        if (p && p->read) {
            b = p->read[pa & 0xFFF];
        } else if (p && p->device) {
            b = p->device->peek(p->device_offset + (pa & 0xFFF), 1);
        } else {
            return false;
        }
        v = (v << 8) | (b & 0xFF);
    }
    *value = v;
    return true;
}

bool Machine::poke(uint32_t ea, unsigned size, uint32_t value) {
    for (unsigned i = 0; i < size; ++i) {
        uint32_t pa = 0;
        if (mmu_translate(state, ea + i, Access::Read, &pa) == Fault::TlbMiss) {
            return false;
        }
        uint8_t* h = host(pa);
        if (!h) {
            return false;
        }
        *h = uint8_t(value >> (8 * (size - 1 - i)));
        code_written(pa);
    }
    return true;
}

// ------------------------------------------------------------------------------------------- running

void Machine::drop_decoded_code() {
    code_.clear();
    code_pages_.reset();
    if (jit_) {
        jit_->clear();
        jit_->collect();
    }
    last_code_page_ = ~0u;
    last_code_ = nullptr;
}

void Machine::reset() {
    reset_core(state);
    for (const auto& d : devices_) {
        d->reset();
    }
    drop_decoded_code();
    tlb_seen_ = state.tlb1;
    pid_seen_ = 0;
    flush_soft_tlbs();
    select_soft_tlb();
    waiting_ = false;
    external_ = critical_ = false;
}

Stop Machine::run_engine() {
    switch (cfg.engine) {
        case Engine::Interpreter:
            return interpret(state, *this);
        case Engine::Block:
            return run_block();
        case Engine::Jit:
            if (jit::host_supported()) {
                return run_jit();
            }
            if (!jit_fallback_warned_) {
                spdlog::warn("e200: no JIT for this host; running the block engine");
                jit_fallback_warned_ = true;
            }
            return run_block();
    }
    return run_block();
}

// ------------------------------------------------------------------------------------------------- the JIT

uint32_t Machine::jit_step(jit::Context* c, const Insn* insn, uint32_t pc, uint32_t k) {
    Machine& m = *static_cast<Machine*>(c->env);
    CpuState& s = m.state;
    s.pc = pc;
    s.cycles = c->start_cycles + k;
    const Stop stop = execute_insn(s, m, *insn);
    if (stop.kind != StopKind::None) {
        m.jit_stop_ = stop;
        return 1;
    }
    // what a chained exit may run up to, as the instruction left it
    c->budget = std::min(m.limit_cycles_, s.timer_event);
    // leave when the instruction went elsewhere (a branch, an interrupt), took extra cycles (lmw/stmw), dropped
    // code, or moved the limit or the decrementer's event inside what is left of the block
    if (s.pc != pc + 4 || s.cycles != c->start_cycles + k + 1 || m.jit_dropped_ || c->budget < c->block_end) {
        return 1;
    }
    return 0;
}

jit::Block* Machine::jit_block(uint32_t ea) {
    uint32_t pa = 0;
    if (exec_physical(ea, &pa) != Fault::None) {
        return nullptr;  // the interpreted step raises the fault
    }
    if (jit::Block* b = jit_->find(ea, pa)) {
        return b;
    }
    std::vector<Insn> insns;
    uint32_t at = ea;
    while (insns.size() < size_t(jit::kMaxBlockInstructions)) {
        Insn in;
        if (next_insn(at, &in) != Fault::None) {
            break;
        }
        insns.push_back(in);
        at += 4;
        if (ends_block(in) || (at & 0xFFF) == 0) {
            break;
        }
    }
    if (insns.empty()) {
        return nullptr;
    }
    return jit_->insert(ea, pa, std::move(insns));
}

Stop Machine::run_jit() {
    CpuState& s = state;
    if (!jit_) {
        jit_ = std::make_unique<jit::JitCache>();
        jit_ctx_.state = &state;
        jit_ctx_.soft = &soft_;
        jit_ctx_.step = &Machine::jit_step;
        jit_ctx_.env = this;
        jit_ctx_.table = jit_->table();
    }
    jit::JitStatistics& js = jit_->stats;
    for (;;) {
        if (s.cycles >= s.timer_event) {
            timer_expired(s);
        }
        if (s.cycles >= limit_cycles_) {
            return Stop{StopKind::Limit, s.pc, 0};
        }
        Stop stop;
        if (take_pending_interrupt(s, *this, &stop)) {
            if (stop.kind != StopKind::None) {
                return stop;
            }
            continue;
        }
        const bool watch = watching();
        if (watch && int64_t(s.pc) != s.skip_watch_pc && pc_watched(s.pc)) {
            return Stop{StopKind::Watch, s.pc, 0};
        }
        s.skip_watch_pc = -1;

        jit_->collect();
        jit::Block* b = watch ? nullptr : jit_block(s.pc);
        if (b && s.cycles + b->count() <= limit_cycles_ && s.cycles + b->count() <= s.timer_event) {
            jit_ctx_.start_cycles = s.cycles;
            jit_ctx_.block_end = s.cycles + b->count();
            jit_ctx_.budget = std::min(limit_cycles_, s.timer_event);
            jit_dropped_ = false;
            jit_stop_ = Stop{};
            b->entry(&jit_ctx_);
            js.blocks_run++;
            if (jit_stop_.kind != StopKind::None) {
                return jit_stop_;
            }
            continue;
        }

        // one instruction, interpreted (the limit or the decrementer is near, a watch is set, or a fetch faults)
        js.interpreted++;
        Insn in;
        const Fault fault = next_insn(s.pc, &in);
        if (fault != Fault::None) {
            s.cycles += 1;
            stop = fetch_fault(s, *this, fault, s.pc);
        } else {
            stop = execute_insn(s, *this, in);
        }
        if (stop.kind != StopKind::None) {
            return stop;
        }
    }
}

Stop Machine::run_block() {
    return run_core(state, *this, [this](uint32_t pc, Insn* in) { return next_insn(pc, in); });
}

Stop Machine::run_until(uint64_t until) {
    select_soft_tlb();
    while (state.cycles < until) {
        if (exit_requested_) {
            return Stop{StopKind::Exit, state.pc, 0};
        }
        if (break_requested_) {
            break_requested_ = false;
            return Stop{StopKind::Break, state.pc, 0};
        }
        schedule_.sync_due(state.cycles);
        uint64_t limit = std::min(until, state.cycles + cfg.max_quantum_cycles);
        limit = std::min(limit, std::max(schedule_.earliest(), state.cycles + 1));
        limit_cycles_ = limit;

        if (waiting_) {
            if (interrupt_ready(state, *this)) {
                waiting_ = false;
                state.msr &= ~kMsrWe;
            } else {
                // asleep: time passes to the quantum's end or the decrementer, whichever is first
                const uint64_t wake = std::min(limit_cycles_, std::max(state.timer_event, state.cycles));
                state.cycles = std::max(state.cycles, wake);
                if (state.cycles >= state.timer_event) {
                    timer_expired(state);
                }
                continue;
            }
        }

        const Stop stop = run_engine();
        switch (stop.kind) {
            case StopKind::Limit:
            case StopKind::None:
                break;
            case StopKind::Wait:
                waiting_ = true;
                break;
            case StopKind::Exit:
                if (exit_requested_) {
                    return stop;
                }
                break;
            case StopKind::Break:
                if (caught_ || break_requested_) {
                    caught_ = false;
                    break_requested_ = false;
                    return stop;
                }
                break;
            case StopKind::Watch:
            case StopKind::Checkstop:
                return stop;
        }
    }
    schedule_.sync_due(state.cycles);
    return Stop{StopKind::Limit, state.pc, 0};
}

// ------------------------------------------------------------------------------------------- debugging

bool Machine::watched_page(uint32_t ea) const {
    const uint32_t page_start = ea & ~0xFFFu;
    for (const auto& [id, w] : data_watches_) {
        if (w.begin < page_start + kPage && w.end > page_start) {
            return true;
        }
    }
    return false;
}

void Machine::check_data_watch(uint32_t ea, unsigned size, uint32_t value, bool write) {
    for (unsigned k = 0; k < size; ++k) {
        const uint32_t a = ea + k;
        const uint8_t byte = uint8_t(value >> (8 * (size - 1 - k)));
        for (auto& [id, w] : data_watches_) {
            if (a < w.begin || a >= w.end || (write ? !w.on_write : !w.on_read)) {
                continue;
            }
            if (w.fn && w.fn(a, byte, write)) {
                request_break();  // after this instruction
            }
        }
    }
}

int Machine::add_data_watch(DataWatch watch) {
    const int id = next_data_watch_++;
    data_watches_.push_back({id, std::move(watch)});
    flush_soft_tlbs();  // the watched pages leave the fast path
    return id;
}

void Machine::remove_data_watch(int id) {
    std::erase_if(data_watches_, [id](const auto& w) { return w.first == id; });
    flush_soft_tlbs();
}

Stop Machine::step_instruction(uint64_t max_ticks, bool* executed) {
    const Engine saved = cfg.engine;
    cfg.engine = Engine::Interpreter;
    const uint64_t before = state.instructions;
    const uint64_t give_up = now() + max_ticks;
    Stop stop{StopKind::Limit, state.pc, 0};
    while (state.instructions == before && now() < give_up) {
        stop = run_until(now() + 1);
        if (stop.kind == StopKind::Watch) {
            state.skip_watch_pc = stop.pc;  // a step passes a watched pc
            continue;
        }
        if (stop.kind != StopKind::Limit && stop.kind != StopKind::Wait && stop.kind != StopKind::None) {
            break;
        }
    }
    cfg.engine = saved;
    *executed = state.instructions != before;
    return stop;
}

Machine::Snapshot Machine::snapshot() const {
    Snapshot s;
    s.state = state;
    for (size_t k = 0; k < storage_.size(); ++k) {
        s.memories.emplace_back(storage_[k].get(), storage_[k].get() + storage_sizes_[k]);
    }
    for (const auto& d : devices_) {
        d->save(s.devices);
    }
    s.external = external_;
    s.critical = critical_;
    s.waiting = waiting_;
    return s;
}

void Machine::restore(const Snapshot& s) {
    state = s.state;
    for (size_t k = 0; k < storage_.size() && k < s.memories.size(); ++k) {
        std::memcpy(storage_[k].get(), s.memories[k].data(), std::min<size_t>(storage_sizes_[k], s.memories[k].size()));
    }
    const uint8_t* p = s.devices.data();
    for (const auto& d : devices_) {
        d->load(p);
    }
    external_ = s.external;
    critical_ = s.critical;
    waiting_ = s.waiting;
    drop_decoded_code();
    tlb_seen_ = state.tlb1;
    pid_seen_ = state.pid0;
    flush_soft_tlbs();
    select_soft_tlb();
}

}  // namespace sim::e200
