#include "hc08/machine.h"

#include <algorithm>
#include <cstring>

#include <spdlog/spdlog.h>

#include "emu/snapshot_io.h"

namespace sim::sys {

Machine::Machine(MachineConfig config) : cfg(config) {
    decode_cache_.on_drop = [this](unsigned page) {
        if (jit_) {
            jit_->drop_page(page);
        }
    };
}

void Machine::add(std::shared_ptr<Periph> device) {
    device->m = this;
    devices_.push_back(device);

    timed_.clear();
    for (const std::shared_ptr<Periph>& p : devices_) {
        if (p->timing() == Timing::Input) {
            timed_.push_back(p.get());
        }
    }
    for (const std::shared_ptr<Periph>& p : devices_) {
        if (p->timing() == Timing::Timed) {
            timed_.push_back(p.get());
        }
    }
}

void Machine::map(uint16_t base, uint32_t size, Periph& device, uint16_t offset) {
    mem.add_device(base, size, &device, offset);
}

Periph* Machine::find(std::string_view name) const {
    for (const std::shared_ptr<Periph>& p : devices_) {
        if (p->name() == name) {
            return p.get();
        }
    }
    return nullptr;
}

void Machine::set_bus_hz(uint64_t hz) {
    const uint32_t tpc = uint32_t(std::max<uint64_t>(1, cfg.tick_hz / hz));
    if (tpc == ticks_per_cycle_) {
        return;
    }

    const uint64_t t = now();
    for (const std::shared_ptr<Periph>& p : devices_) {
        p->sync(t);
    }

    base_tick_ = t;
    base_cycles_ = cpu.cycles;
    ticks_per_cycle_ = tpc;
    limit_cycles_ = std::min(limit_cycles_, cpu.cycles);  // end the quantum: its cycle budget was at the old rate

    for (const std::shared_ptr<Periph>& p : devices_) {
        p->bus_clock_changed(t);
    }

    SPDLOG_INFO("bus clock {} Hz ({} ticks per cycle) at t={:.6f} s", hz, tpc, double(t) / double(cfg.tick_hz));
}

void Machine::set_line(int source, bool on) {
    const uint32_t bit = 1u << source;
    irq_lines_ = on ? (irq_lines_ | bit) : (irq_lines_ & ~bit);
}

void Machine::request_reset(ResetCause cause) {
    reset_pending_ = true;
    reset_cause_ = cause;
    limit_cycles_ = cpu.cycles;
}

void Machine::reset(ResetCause cause) {
    reset_count_++;
    const uint64_t t = now();
    for (const std::shared_ptr<Periph>& p : devices_) {
        p->sync(t);
        p->reset();
    }

    irq_lines_ = 0;
    mode_ = CpuMode::Run;
    cpu.a = 0;
    cpu.hx = 0;
    cpu.sp = 0x00FF;
    cpu.ccr = ccr::kOnes | ccr::kI;
    cpu.irq_inhibit = false;
    cpu.skip_watch_pc = -1;
    cpu.pc = uint16_t((uint16_t(mem.bytes[0xFFFE]) << 8) | mem.bytes[0xFFFF]);
    base_tick_ = t;
    base_cycles_ = cpu.cycles;

    if (on_reset) {
        on_reset(*this, cause);
    }
}

void Machine::flash_changed(uint16_t addr, uint32_t size) {
    for (uint32_t k = 0; k < size; k += 0x100) {
        decode_cache_.invalidate(uint16_t(addr + k));
    }
    decode_cache_.invalidate(uint16_t(addr + size - 1));
    limit_cycles_ = std::min(limit_cycles_, cpu.cycles);
}

// ---------------------------------------------------------------------------------------------- device accesses

uint8_t Machine::device_read(uint16_t addr) {
    Periph& device = static_cast<Periph&>(*mem.slot(addr).device);
    device.sync(now());
    const uint8_t value = device.read(mem.device_offset(addr));
    after_device_access(device);
    return value;
}

void Machine::device_write(uint16_t addr, uint8_t value) {
    Periph& device = static_cast<Periph&>(*mem.slot(addr).device);
    device.sync(now());
    device.write(mem.device_offset(addr), value);
    after_device_access(device);
}

void Machine::after_device_access(Periph& device) {
    // an event moved before the quantum's end: end the quantum after this instruction, so it is processed on time
    if (device.next_event() < quantum_end_) {
        limit_cycles_ = std::min(limit_cycles_, cpu.cycles);
    }
}

uint8_t Machine::peek(uint16_t addr) {
    if (mem.region(addr) == Region::Device) {
        Periph& device = static_cast<Periph&>(*mem.slot(addr).device);
        device.sync(now());
        return device.peek(mem.device_offset(addr));
    }
    return mem.bytes[addr];
}

void Machine::poke(uint16_t addr, uint8_t value) {
    const Region region = mem.region(addr);
    if (region == Region::Device) {
        device_write(addr, value);
        return;
    }

    mem.bytes[addr] = value;
    if (decode_cache_.has_code(addr)) {
        decode_cache_.invalidate(addr);
    }
}

// ---------------------------------------------------------------------------------------------- watches

void Machine::watch_pc(uint16_t pc, bool on) {
    uint64_t& word = pc_watch_[pc >> 6];
    const uint64_t bit = uint64_t(1) << (pc & 63);
    const bool was = (word & bit) != 0;

    if (on && !was) {
        word |= bit;
        pc_watch_count_++;
    } else if (!on && was) {
        word &= ~bit;
        pc_watch_count_--;
    }
}

void Machine::clear_pc_watches() {
    std::fill(pc_watch_.begin(), pc_watch_.end(), 0);
    pc_watch_count_ = 0;
}

int Machine::add_data_watch(DataWatch watch) {
    const int id = next_watch_id_++;
    data_watches_.emplace_back(id, std::move(watch));
    return id;
}

void Machine::remove_data_watch(int id) {
    std::erase_if(data_watches_, [id](const auto& entry) { return entry.first == id; });
}

void Machine::check_data_watch(uint16_t addr, uint8_t value, bool write) {
    for (auto& [id, watch] : data_watches_) {
        if (addr < watch.begin || addr >= watch.end) {
            continue;
        }
        if (write ? !watch.on_write : !watch.on_read) {
            continue;
        }
        if (watch.fn && watch.fn(addr, value, write)) {
            request_break();
        }
    }
}

// ---------------------------------------------------------------------------------------------- the run loop

void Machine::sync_due(uint64_t t) {
    // repeated passes: a device synced may make another due (the stimulus sets an input the ADC samples)
    for (int pass = 0; pass < 64; pass++) {
        bool any = false;
        for (Periph* p : timed_) {
            if (p->next_event() <= t) {
                p->sync(t);
                any = true;
            }
        }
        if (!any) {
            return;
        }
    }
    SPDLOG_WARN("devices still due after 64 passes at t={}", t);
}

uint64_t Machine::earliest_event() const {
    uint64_t earliest = kNever;
    for (const Periph* p : timed_) {
        earliest = std::min(earliest, p->next_event());
    }
    return earliest;
}

Stop Machine::run_engine() {
    switch (cfg.engine) {
        case CpuEngine::Interpreter:
            return interpret(cpu, *this);
        case CpuEngine::Jit:
            // watches see every instruction and access: the Block engine runs while any is set (as in mk60e5)
            if (jit::host_supported() && pc_watch_count_ == 0 && data_watches_.empty()) {
                if (!jit_) {
                    jit_ = std::make_unique<jit::JitCache>();
                }
                return jit::run_jit(cpu, *this, decode_cache_, *jit_);
            }
            if (!jit::host_supported() && !jit_fallback_warned_) {
                jit_fallback_warned_ = true;
                SPDLOG_WARN("no JIT assembler for this host: running the block engine");
            }
            [[fallthrough]];
        case CpuEngine::Block:
            return run_blocks(cpu, *this, decode_cache_);
    }
    return Stop{StopKind::Limit, cpu.pc, 0};
}

void Machine::enter_mode(CpuMode mode) {
    const uint64_t t = now();
    const bool was_stop = mode_ == CpuMode::Stop;
    const bool is_stop = mode == CpuMode::Stop;
    mode_ = mode;

    if (was_stop != is_stop) {
        for (const std::shared_ptr<Periph>& p : devices_) {
            p->sync(t);
            p->stop_mode(is_stop, t);
        }
    }
}

Stop Machine::run_until(uint64_t until) {
    break_requested_ = false;

    while (now() < until) {
        if (exit_requested_.load(std::memory_order_relaxed)) {
            return Stop{StopKind::Exit, cpu.pc, 0};
        }

        if (reset_pending_) {
            reset_pending_ = false;
            reset(reset_cause_);
        }

        const uint64_t t = now();
        sync_due(t);

        // a sleeping CPU wakes on an interrupt it can see
        if (mode_ != CpuMode::Run) {
            const uint32_t wake = mode_ == CpuMode::Stop ? (irq_lines_ & irq::kStopWakeMask) : irq_lines_;
            if (wake != 0) {
                enter_mode(CpuMode::Run);
            }
        }

        const uint64_t end = std::min({until, earliest_event(), t + cfg.max_quantum_ticks});

        if (mode_ != CpuMode::Run) {
            // nothing runs: time jumps to the next event
            base_tick_ = std::max(end, t + 1);
            base_cycles_ = cpu.cycles;
            continue;
        }

        quantum_end_ = end;
        base_tick_ = t;
        base_cycles_ = cpu.cycles;
        limit_cycles_ = cpu.cycles + std::max<uint64_t>(1, (end - t + ticks_per_cycle_ - 1) / ticks_per_cycle_);

        const Stop stop = run_engine();
        quantum_end_ = kNever;

        switch (stop.kind) {
            case StopKind::Wait:
                enter_mode(CpuMode::Wait);
                break;
            case StopKind::StopMode:
                enter_mode(CpuMode::Stop);
                break;
            case StopKind::Illegal:
                SPDLOG_WARN("illegal opcode ${:04x} at pc=${:04x}: reset (ILOP)", stop.detail, stop.pc);
                request_reset(ResetCause::IllegalOpcode);
                break;
            case StopKind::Watch:
                return stop;
            case StopKind::Limit:
                if (break_requested_) {
                    break_requested_ = false;
                    return Stop{StopKind::Break, cpu.pc, 0};
                }
                break;
            case StopKind::None:
            case StopKind::Break:
            case StopKind::Exit:
                break;
        }
    }

    if (reset_pending_) {
        reset_pending_ = false;
        reset(reset_cause_);
    }
    sync_due(now());
    return Stop{StopKind::Limit, cpu.pc, 0};
}

// ---------------------------------------------------------------------------------------------- snapshots

Machine::Snapshot Machine::snapshot() const {
    Snapshot s;
    s.cpu = cpu;
    s.memory.assign(mem.bytes.begin(), mem.bytes.end());
    for (const std::shared_ptr<Periph>& p : devices_) {
        p->save(s.devices);
    }
    s.base_tick = base_tick_;
    s.base_cycles = base_cycles_;
    s.ticks_per_cycle = ticks_per_cycle_;
    s.irq_lines = irq_lines_;
    s.mode = mode_;
    return s;
}

void Machine::restore(const Snapshot& s) {
    cpu = s.cpu;
    std::memcpy(mem.bytes.data(), s.memory.data(), mem.bytes.size());
    const uint8_t* in = s.devices.data();
    for (const std::shared_ptr<Periph>& p : devices_) {
        p->load(in);
    }
    base_tick_ = s.base_tick;
    base_cycles_ = s.base_cycles;
    ticks_per_cycle_ = s.ticks_per_cycle;
    irq_lines_ = s.irq_lines;
    mode_ = s.mode;
    decode_cache_.clear();
}

}  // namespace sim::sys
