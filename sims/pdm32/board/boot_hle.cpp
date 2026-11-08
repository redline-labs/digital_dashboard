#include "pdm32/board/boot_hle.h"

#include <spdlog/spdlog.h>

namespace sim::sys {

namespace {
constexpr uint16_t kRedirectTable = 0xC33B;
constexpr uint16_t kFirstRedirectedVector = 0xFFCC;
constexpr uint16_t kLastRedirectedVector = 0xFFFC;  // the last of the 25 redirected vectors
constexpr uint16_t kAppEntry = 0x8006;
constexpr uint16_t kHardwareRevision = 0xFD80;
}  // namespace

void install_boot_stand_in(Pdm32Board& board) {
    Machine& m = board.soc.m;
    auto& bytes = m.mem.bytes;

    for (uint16_t v = kFirstRedirectedVector, k = 0; v <= kLastRedirectedVector; v += 2, k++) {
        const uint16_t target = uint16_t(kRedirectTable + 3 * k);
        bytes[v] = uint8_t(target >> 8);
        bytes[v + 1] = uint8_t(target);
    }
    bytes[0xFFFE] = uint8_t(kAppEntry >> 8);
    bytes[0xFFFF] = uint8_t(kAppEntry);
    bytes[kHardwareRevision] = 0;
    bytes[0xFF80] = 0xFF;
    bytes[0xFF81] = 0xFF;

    if (bytes[kRedirectTable] != 0xCC) {
        SPDLOG_WARN("boot: no JMP at the vector redirect table ${:04x}: not the v1.91U image?", kRedirectTable);
    }

    m.on_reset = [&board](Machine& machine, ResetCause cause) {
        board.soc.cgm->set_bootloader_state();
        board.soc.sim->set_reset_cause(uint8_t(cause));
        machine.cpu.sp = 0x00FF;
        SPDLOG_INFO("boot: reset ({}) -> ${:04x}",
                    cause == ResetCause::PowerOn ? "power-on"
                    : cause == ResetCause::Cop   ? "COP"
                                                 : "other",
                    machine.cpu.pc);
    };
}

}  // namespace sim::sys
