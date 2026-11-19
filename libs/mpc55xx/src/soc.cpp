#include "mpc55xx/soc.h"

#include <spdlog/spdlog.h>

namespace sim::mpc55xx {

Mpc5554::Mpc5554(e200::Machine& machine, SocConfig config) : m(machine), cfg(config) {
    flash = m.map_rom(map::kFlash, map::kFlashSize);
    sram = m.map_ram(map::kSram, map::kSramSize);

    intc = std::make_shared<Intc>();
    m.add(intc);
    m.map(map::kIntc, 0x4000, *intc);

    const uint32_t bases[3] = {map::kFlexCanA, map::kFlexCanB, map::kFlexCanC};
    const FlexCanSources sources[3] = {kFlexCanASources, kFlexCanBSources, kFlexCanCSources};
    const char* names[3] = {"flexcan_a", "flexcan_b", "flexcan_c"};
    for (size_t k = 0; k < 3; ++k) {
        can[k] = std::make_shared<FlexCan>(names[k], cfg.system_hz, cfg.oscillator_hz);
        m.add(can[k]);
        m.map(bases[k], 0x4000, *can[k]);
        can[k]->attach(can_bus[k]);
        for (int b = 0; b < 16; ++b) {
            can[k]->buffer_irq[size_t(b)] = intc->line(sources[k].buffer0 + b);
        }
        can[k]->buffers_16_31_irq = intc->line(sources[k].buffers_16_31);
        can[k]->buffers_32_63_irq = intc->line(sources[k].buffers_32_63);
    }

    esci_a = std::make_shared<Esci>("esci_a");
    m.add(esci_a);
    m.map(map::kEsciA, 0x4000, *esci_a);

    // the modules not modelled: storage, with what their status registers must say
    add_stub("fmpll", map::kFmpll, 0x4000, {{0x04, 0x00000008}});  // SYNSR: LOCK
    add_stub("flash_ctrl", map::kFlashController, 0x4000);
    add_stub("siu", map::kSiu, 0x4000, {{0x04, 0x55540000}});      // MIDR: MPC5554
    add_stub("emios", map::kEmios, 0x4000);
    add_stub("etpu", map::kEtpu, 0x4000);
    add_stub("etpu_ram", map::kEtpuRam, 0x4000);
    add_stub("etpu_code", map::kEtpuCode, 0x8000);
    add_stub("pbridge_a", map::kBridgeA, 0x4000);
    add_stub("pbridge_b", map::kBridgeB, 0x4000);
    add_stub("xbar", map::kXbar, 0x4000);
    add_stub("ecsm", map::kEcsm, 0x4000);
    add_stub("edma", map::kEdma, 0x4000);
    add_stub("eqadc", map::kEqadc, 0x4000);
    add_stub("dspi_a", map::kDspiA, 0x4000);
    add_stub("dspi_b", map::kDspiB, 0x4000);
    add_stub("dspi_c", map::kDspiC, 0x4000);
    add_stub("dspi_d", map::kDspiD, 0x4000);
    add_stub("esci_b", map::kEsciB, 0x4000);
}

void Mpc5554::add_stub(std::string name, uint32_t base, uint32_t size, std::vector<StubModule::Fixed> fixed) {
    auto stub = std::make_shared<StubModule>(std::move(name), size, std::move(fixed));
    m.add(stub);
    m.map(base, size, *stub);
    stubs.push_back(std::move(stub));
}

bool Mpc5554::load_flash(const std::vector<uint8_t>& image) {
    if (image.size() > map::kFlashSize) {
        return false;
    }
    return m.load_image(map::kFlash, image.data(), image.size());
}

bool Mpc5554::reset() {
    m.reset();
    return bam_boot();
}

bool Mpc5554::bam_boot() {
    e200::CpuState& s = m.state;
    // the BAM's TLB entries: every access, the peripheral bridges cache-inhibited and guarded
    s.tlb1[0] = e200::TlbEntry{0xC0000500, map::kBridgeB | 0x0A, map::kBridgeB | 0x3F};  // 1 MB
    s.tlb1[1] = e200::TlbEntry{0xC0000700, map::kFlash, map::kFlash | 0x3F};               // 16 MB
    s.tlb1[2] = e200::TlbEntry{0xC0000700, map::kExternal, map::kExternal | 0x3F};         // 16 MB
    s.tlb1[3] = e200::TlbEntry{0xC0000400, map::kSram | 0x08, map::kSram | 0x3F};          // 256 KB
    s.tlb1[4] = e200::TlbEntry{0xC0000500, map::kBridgeA | 0x0A, map::kBridgeA | 0x3F};  // 1 MB
    m.translation_changed();

    // the reset configuration half word: boot identifier 0x5A in its low byte, then the reset vector
    const uint32_t rchw = (uint32_t(flash[0]) << 24) | (uint32_t(flash[1]) << 16) | (uint32_t(flash[2]) << 8) | flash[3];
    if (((rchw >> 16) & 0xFF) != 0x5A) {
        spdlog::warn("mpc5554: no valid reset configuration in flash (0x{:08x}); the core stays at the reset vector",
                     rchw);
        return false;
    }
    s.pc = (uint32_t(flash[4]) << 24) | (uint32_t(flash[5]) << 16) | (uint32_t(flash[6]) << 8) | flash[7];
    return true;
}

}  // namespace sim::mpc55xx
