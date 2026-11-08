#include "pdm32/config/current_cal.h"

#include <stdexcept>

#include "pdm32/config/crc16.h"
#include "pdm32/config/current_cal_dumped.h"
#include "pdm32/config/layout.h"

namespace pdm::config {

namespace {

constexpr size_t kBlockBytes = kCurrentCalBlockBytes;
constexpr CurrentCal kUnity = unity_current_cal();

}  // namespace

std::vector<uint8_t> encode_current_cal(const CurrentCal& cal) {
    std::vector<uint8_t> block(kBlockBytes, 0);
    block[0] = 0;
    block[1] = 1;  // format
    block[2] = uint8_t(kBlockBytes >> 8);
    block[3] = uint8_t(kBlockBytes);
    for (size_t k = 0; k < layout::kOutputs; k++) {
        const OutputCurrentCal& o = cal.outputs[k];
        uint8_t* e = &block[4 + k * layout::kCalEntryBytes];
        std::copy(o.low_lut.begin(), o.low_lut.end(), e);
        e[32] = uint8_t(o.gain >> 8);
        e[33] = uint8_t(o.gain);
        e[34] = uint8_t(uint16_t(o.offset) >> 8);
        e[35] = uint8_t(o.offset);
    }
    const uint16_t crc = crc16_ccitt(std::span<const uint8_t>(block.data(), block.size() - 2));
    block[kBlockBytes - 2] = uint8_t(crc >> 8);
    block[kBlockBytes - 1] = uint8_t(crc);
    return block;
}

CurrentCal decode_current_cal(std::span<const uint8_t> block) {
    if (block.size() < kBlockBytes || block[0] != 0 || block[1] != 1 ||
        ((size_t(block[2]) << 8) | block[3]) != kBlockBytes) {
        throw std::runtime_error("current calibration: not a format-1 block of the PDM32's length");
    }
    const uint16_t crc = crc16_ccitt(block.first(kBlockBytes - 2));
    if (((uint16_t(block[kBlockBytes - 2]) << 8) | block[kBlockBytes - 1]) != crc) {
        throw std::runtime_error("current calibration: CRC mismatch");
    }

    CurrentCal cal;
    for (size_t k = 0; k < layout::kOutputs; k++) {
        OutputCurrentCal& o = cal.outputs[k];
        const uint8_t* e = &block[4 + k * layout::kCalEntryBytes];
        std::copy(e, e + 32, o.low_lut.begin());
        o.gain = uint16_t((e[32] << 8) | e[33]);
        o.offset = int16_t(uint16_t((e[34] << 8) | e[35]));
    }
    return cal;
}

const CurrentCal& builtin_current_cal(const std::string& name) {
    if (name == "dumped") {
        return kCurrentCalDumped;
    }
    if (name == "unity") {
        return kUnity;
    }
    throw std::runtime_error("current calibration '" + name + "': unknown (dumped, unity)");
}

}  // namespace pdm::config
