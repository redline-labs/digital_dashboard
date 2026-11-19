#include "m150/board/m150.h"

#include <fstream>
#include <iterator>
#include <stdexcept>

namespace sim::m150 {

namespace {
e200::MachineConfig with_core_clock(e200::MachineConfig config) {
    config.core_hz = 125'000'000;  // the M150's system clock (the firmware's 125 MHz timebase)
    return config;
}
}  // namespace

Board::Board(e200::MachineConfig config)
    : m(with_core_clock(config)), soc(m, mpc55xx::SocConfig{}), topology(topology::m150()) {
    external_ram = m.map_ram(kExternalRam, kExternalRamSize);
    fpga = std::make_shared<mpc55xx::StubModule>("fpga", 0x10000);
    m.add(fpga);
    m.map(kFpga, 0x10000, *fpga);
}

void Board::load_flash_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read " + path);
    }
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!soc.load_flash(image)) {
        throw std::runtime_error(path + ": larger than the internal flash (2 MB)");
    }
}

}  // namespace sim::m150
