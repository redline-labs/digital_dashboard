// The M150 board: its MPC5554 (libs/mpc55xx on an e200 machine), the external RAM on the external bus, the FPGA's
// window (not modelled: storage), and the board's topology (board/topology).
//
// What is known of the board's memory is the M1 SDK's map (M150_FINDINGS in the research tree): external RAM at
// 0x20000000, the FPGA at 0x2F000000, external NOR flash at 0x30000000. The external RAM's size is not known; 4 MB
// is assumed. The NOR flash is not mapped.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "e200/machine.h"
#include "m150/board/topology/topology.h"
#include "mpc55xx/soc.h"

namespace sim::m150 {

inline constexpr uint32_t kExternalRam = 0x20000000;
inline constexpr uint32_t kExternalRamSize = 0x00400000;
inline constexpr uint32_t kFpga = 0x2F000000;

class Board {
   public:
    explicit Board(e200::MachineConfig config = {});

    e200::Machine m;
    mpc55xx::Mpc5554 soc;
    uint8_t* external_ram = nullptr;
    std::shared_ptr<mpc55xx::StubModule> fpga;
    const topology::Topology& topology;

    // A raw image of the internal flash (as the part holds it, from address 0). Throws std::runtime_error when it
    // cannot be read or does not fit.
    void load_flash_file(const std::string& path);
    // Reset and boot (mpc55xx::Mpc5554::reset). False when flash holds no valid reset configuration.
    bool reset() {
        return soc.reset();
    }
};

}  // namespace sim::m150
