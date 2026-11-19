// The MPC5554 around an e200 machine: internal flash and SRAM, the peripheral bridges with the modules the simulator
// models (INTC, FlexCAN A-C, eSCI A as a console) and stubs for the rest, and the boot ROM's work at reset.
//
// The boot assist module (BAM) is the part's boot ROM; it is not in the tree and does not run. bam_boot() does what it
// does for a boot from internal flash (MPC5554 RM, chapter 16): the five TLB entries it leaves (peripheral bridge B,
// internal flash, the external bus, SRAM, peripheral bridge A), the reset configuration half word at the start of
// flash checked for its boot identifier, and a jump to the reset vector the word after it holds. Booting from the
// external bus or serially is not modelled.
//
// Which MPC55xx the M150 carries is not confirmed; this is the MPC5554's memory map (MPC5554 RM, table 1-2).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "e200/machine.h"
#include "emu/can_bus.h"
#include "mpc55xx/esci.h"
#include "mpc55xx/flexcan.h"
#include "mpc55xx/intc.h"
#include "mpc55xx/registers.h"

namespace sim::mpc55xx {

namespace map {
inline constexpr uint32_t kFlash = 0x00000000, kFlashSize = 0x00200000;
inline constexpr uint32_t kExternal = 0x20000000;
inline constexpr uint32_t kSram = 0x40000000, kSramSize = 0x00010000;
inline constexpr uint32_t kBridgeA = 0xC3F00000;
inline constexpr uint32_t kFmpll = 0xC3F80000, kFlashController = 0xC3F88000, kSiu = 0xC3F90000,
                          kEmios = 0xC3FA0000, kEtpu = 0xC3FC0000, kEtpuRam = 0xC3FC8000, kEtpuCode = 0xC3FD0000;
inline constexpr uint32_t kBridgeB = 0xFFF00000;
inline constexpr uint32_t kXbar = 0xFFF04000, kEcsm = 0xFFF40000, kEdma = 0xFFF44000, kIntc = 0xFFF48000,
                          kEqadc = 0xFFF80000, kDspiA = 0xFFF90000, kDspiB = 0xFFF94000, kDspiC = 0xFFF98000,
                          kDspiD = 0xFFF9C000, kEsciA = 0xFFFB0000, kEsciB = 0xFFFB4000, kFlexCanA = 0xFFFC0000,
                          kFlexCanB = 0xFFFC4000, kFlexCanC = 0xFFFC8000;
}  // namespace map

// INTC sources (MPC5554 RM, table 10-9): each FlexCAN's buffers 0..15, then 16..31 and 32..63.
struct FlexCanSources {
    int buffer0;
    int buffers_16_31;
    int buffers_32_63;
};
inline constexpr FlexCanSources kFlexCanASources{155, 171, 172};
inline constexpr FlexCanSources kFlexCanBSources{283, 299, 300};
inline constexpr FlexCanSources kFlexCanCSources{176, 192, 193};

struct SocConfig {
    uint64_t system_hz = 125'000'000;
    uint64_t oscillator_hz = 8'000'000;
};

class Mpc5554 {
   public:
    // Maps the part onto `m` (whose core clock is the system clock).
    Mpc5554(e200::Machine& m, SocConfig config = {});

    e200::Machine& m;
    SocConfig cfg;

    uint8_t* flash = nullptr;
    uint8_t* sram = nullptr;
    std::shared_ptr<Intc> intc;
    std::array<std::shared_ptr<FlexCan>, 3> can;  // A, B, C
    std::array<sys::CanBus, 3> can_bus;           // the bus each controller is on
    std::shared_ptr<Esci> esci_a;
    std::vector<std::shared_ptr<StubModule>> stubs;

    // Loads a flash image at 0 (a raw image, as the part's flash holds it). False if it does not fit.
    bool load_flash(const std::vector<uint8_t>& image);

    // Reset: the machine and its devices, then the boot ROM's work (bam_boot). False when flash holds no valid reset
    // configuration (the part would then try the other boot modes: the core is left at the reset vector).
    bool reset();
    bool bam_boot();

   private:
    void add_stub(std::string name, uint32_t base, uint32_t size, std::vector<StubModule::Fixed> fixed = {});
};

}  // namespace sim::mpc55xx
