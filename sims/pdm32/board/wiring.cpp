#include "pdm32/board/wiring.h"

namespace sim::sys::pdm32 {

const std::array<Pin, 32> kOutputPins = {{
    {Port::D, 6}, {Port::D, 7},                                                          // O1-O2
    {Port::E, 0}, {Port::E, 1}, {Port::E, 2}, {Port::E, 3}, {Port::E, 4}, {Port::E, 5},  // O3-O8
    {Port::F, 4}, {Port::F, 5}, {Port::F, 6}, {Port::F, 7},                              // O9-O12
    {Port::D, 4}, {Port::D, 5},                                                          // O13-O14
    {Port::F, 0}, {Port::F, 1}, {Port::F, 2}, {Port::F, 3},                              // O15-O18
    {Port::A, 2}, {Port::A, 3}, {Port::A, 4}, {Port::A, 5}, {Port::A, 6}, {Port::A, 7},  // O19-O24
    {Port::C, 2}, {Port::C, 3}, {Port::C, 4}, {Port::C, 5}, {Port::C, 6},                // O25-O29
    {Port::D, 1}, {Port::D, 2}, {Port::D, 3},                                            // O30-O32
}};

const std::array<uint8_t, 360> kAdcScanTable = {
#include "pdm32/board/scan_table.inc"
};

namespace {

// the RAM destinations of the scan (config/layout.h has them too; board/ may not include config/)
constexpr uint16_t kBatteryRaw = 0x0116;
constexpr uint16_t kRailRaw = 0x0117;
constexpr uint16_t kTempRaw = 0x0303;
constexpr uint16_t kInputVoltage = 0x01E4;
constexpr uint16_t kOutputVoltage = 0x01FB;
constexpr uint16_t kOutputCurrent = 0x021B;

struct SenseMap {
    std::array<SenseSource, 32 * 32> map{};

    SenseMap() {
        for (size_t e = 0; e < kAdcScanTable.size(); e += 4) {
            const uint8_t mux = kAdcScanTable[e] & 0x13;
            const int channel = kAdcScanTable[e + 1] & 0x1F;
            const uint16_t dest = uint16_t((kAdcScanTable[e + 2] << 8) | kAdcScanTable[e + 3]);
            SenseSource s;
            if (dest == kBatteryRaw) {
                s.kind = Sense::Battery;
            } else if (dest == kRailRaw) {
                s.kind = Sense::Rail9V5;
            } else if (dest == kTempRaw) {
                s.kind = Sense::BoardTemp;
            } else if (dest >= kInputVoltage && dest < kInputVoltage + 23) {
                s = {Sense::InputVoltage, uint8_t(dest - kInputVoltage)};
            } else if (dest >= kOutputVoltage && dest < kOutputVoltage + 32) {
                s = {Sense::OutputVoltage, uint8_t(dest - kOutputVoltage)};
            } else if (dest >= kOutputCurrent && dest < kOutputCurrent + 32) {
                s = {Sense::OutputCurrent, uint8_t(dest - kOutputCurrent)};
            }
            map[size_t(channel) * 32 + mux] = s;
        }
        // the battery channel reads the battery whatever the address (the scan's first, discarded conversion)
        for (uint8_t mux = 0; mux < 32; mux++) {
            if (map[2 * 32 + mux].kind == Sense::None) {
                map[2 * 32 + mux] = {Sense::Battery, 0};
            }
        }
    }
};

const SenseMap& sense_map() {
    static const SenseMap instance;
    return instance;
}

}  // namespace

SenseSource sense_source(int channel, uint8_t mux) {
    if (channel < 0 || channel >= 32) {
        return {};
    }
    return sense_map().map[size_t(channel) * 32 + (mux & 0x13)];
}

}  // namespace sim::sys::pdm32
