// What the debugger needs of a simulated device, whatever its CPU: the CPU's registers and memory as the debugger
// sees them, decoding (from the same tables the CPU's engines run), pc and data watches, running and single-stepping,
// the call stack, snapshots, symbols, and the device's own names (a connector's pins, a device register).
//
// The shared debugger (host/debug/debugger.h), session (session.h) and agent methods (methods.h) are written against
// this alone, so every simulator has the same debugging interface. Addresses are 64-bit here; a target uses the
// width its CPU has.
#pragma once

#include <any>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "host/debug/symbols.h"

namespace sim::sys {

// Why a run returned.
enum class TargetStopKind : uint8_t {
    Time,       // the time asked for was reached (or the CPU is asleep until after it)
    PcWatch,    // a watched pc was reached (not executed)
    DataWatch,  // a data watch's callback asked to stop (after the access's instruction)
    Exit,       // the run was asked to end
    Fault,      // the CPU stopped and cannot go on (a checkstop)
};

struct TargetStop {
    TargetStopKind kind = TargetStopKind::Time;
    uint64_t pc = 0;
};

// One decoded instruction, as the debugger needs it.
struct Decoded {
    std::string text;      // "addi r3,r1,8"
    uint32_t length = 1;   // bytes
    bool call = false;     // a call that returns to pc + length (JSR/BSR; a branch that sets LR)
    bool ret = false;      // a return from a call (RTS; blr)
    bool valid = true;     // false where nothing could be read
};

class DebugTarget {
   public:
    virtual ~DebugTarget() = default;

    // "hc08", "e200"; hex digits of an address (4, 8)
    virtual std::string cpu() const = 0;
    virtual int address_digits() const = 0;

    // ---- time
    virtual double seconds() const = 0;
    virtual uint64_t now() const = 0;  // ticks
    virtual uint64_t ticks(double seconds) const = 0;

    // ---- the CPU
    virtual uint64_t pc() const = 0;
    virtual uint64_t stack_pointer() const = 0;
    // every register, by its assembler name
    virtual nlohmann::json registers() const = 0;
    virtual std::optional<uint64_t> read_register(const std::string& name) const = 0;
    virtual bool write_register(const std::string& name, uint64_t value) = 0;

    // Memory as the CPU sees it now, without side effects (a device register is peeked). False where nothing is.
    virtual bool peek(uint64_t address, uint8_t* out, size_t size) = 0;
    // Writes memory (RAM; the debugger's write). False where it cannot.
    virtual bool poke(uint64_t address, const uint8_t* in, size_t size) = 0;
    virtual Decoded decode(uint64_t address) = 0;

    // ---- running
    virtual void watch_pc(uint64_t pc, bool on) = 0;
    // the next run passes a watch at `pc` once (resuming from a stop there)
    virtual void skip_pc_once(uint64_t pc) = 0;
    // `fn(address, value, is_write)` for each byte accessed in [begin, end); true stops the run after the instruction
    using DataWatchFn = std::function<bool(uint64_t address, uint8_t value, bool write)>;
    virtual int add_data_watch(uint64_t begin, uint64_t end, bool on_read, bool on_write, DataWatchFn fn) = 0;
    virtual void remove_data_watch(int id) = 0;
    virtual TargetStop run_until(uint64_t tick) = 0;
    // Exactly one instruction (an interrupt due first is entered as the CPU would); asleep, time passes until it
    // wakes, at most `max_ticks`. Returns how it ended: Time when one instruction ran (or the wait ran out).
    virtual TargetStop step_instruction(uint64_t max_ticks) = 0;
    // Whether the last step_instruction ran an instruction (false: the CPU slept through max_ticks).
    virtual bool stepped() const = 0;
    virtual void clear_exit() = 0;
    // Power-on reset (the debugger's breakpoints and watches stay).
    virtual void reset() = 0;

    // Return addresses on the stack, innermost first (a heuristic each CPU makes its own way).
    virtual std::vector<uint64_t> return_addresses() = 0;

    // ---- snapshots: the whole device (CPU, memories, devices, the board's world)
    virtual std::any snapshot() = 0;
    virtual void restore(const std::any& snapshot) = 0;

    // ---- names
    virtual const Symbols& symbols() const = 0;
    // The device's own names (host/debug/names.h lists the shared ones). Unknown: nullopt with `error` empty.
    virtual std::optional<double> read_device_name(const std::string& name, std::string& error) {
        (void)name;
        (void)error;
        return std::nullopt;
    }
    // Sets a device name: nullopt when the name is not the device's, "" when done, else the error.
    virtual std::optional<std::string> write_device_name(const std::string& name, const nlohmann::json& value) {
        (void)name;
        (void)value;
        return std::nullopt;
    }
};

}  // namespace sim::sys
