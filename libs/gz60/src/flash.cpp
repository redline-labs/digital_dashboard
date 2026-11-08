#include "gz60/flash.h"

#include <spdlog/spdlog.h>

#include "hc08/machine.h"
#include "emu/model_gap.h"
#include "emu/snapshot_io.h"

namespace sim::sys {

namespace {
constexpr uint8_t kPgm = 0x01;
constexpr uint8_t kErase = 0x02;
constexpr uint8_t kMass = 0x04;
constexpr uint8_t kHven = 0x08;
constexpr DeviceRegister kRegisters[] = {{"FL1CR", 0}, {"FL2CR", 1}};

struct Range {
    uint16_t begin;
    uint32_t end;  // exclusive
};
constexpr Range kFlash1[] = {{0x8000, 0xFE00}, {0xFF80, 0x10000}};
constexpr Range kFlash2[] = {{0x0462, 0x0500}, {0x0980, 0x1B80}, {0x1E20, 0x8000}};

model_gap::Once<2> stray_write;
}  // namespace

std::span<const DeviceRegister> FlashController::registers() const {
    return kRegisters;
}

std::optional<size_t> FlashController::array_of(uint16_t addr) {
    for (const Range& r : kFlash1) {
        if (addr >= r.begin && addr < r.end) {
            return 0;
        }
    }
    for (const Range& r : kFlash2) {
        if (addr >= r.begin && addr < r.end) {
            return 1;
        }
    }
    return std::nullopt;
}

void FlashController::reset() {
    cr_.fill(0);
    latch_ = {-1, -1};
}

uint8_t FlashController::rd(uint16_t offset) {
    return offset < 2 ? cr_[offset] : 0;
}

void FlashController::wr(uint16_t offset, uint8_t value) {
    if (offset >= 2) {
        return;
    }
    const uint8_t old = cr_[offset];
    cr_[offset] = uint8_t(value & 0x0F);

    // high voltage switched on with ERASE: the latched page (or the array) is erased
    if (!(old & kHven) && (value & kHven) && (value & kErase) && latch_[offset] >= 0) {
        erase(offset, uint16_t(latch_[offset]), (value & kMass) != 0);
    }
    if (!(value & (kPgm | kErase))) {
        latch_[offset] = -1;
    }
}

void FlashController::erase(size_t array, uint16_t latched, bool mass) {
    erases_++;
    if (mass) {
        const auto erase_ranges = [this](const auto& ranges) {
            for (const Range& r : ranges) {
                for (uint32_t a = r.begin; a < r.end; a++) {
                    m->mem.bytes[a] = 0xFF;
                }
                m->flash_changed(r.begin, r.end - r.begin);
            }
        };
        if (array == 0) {
            erase_ranges(kFlash1);
        } else {
            erase_ranges(kFlash2);
        }
        SPDLOG_INFO("flash: mass erase of FLASH-{}", array + 1);
        return;
    }

    const uint16_t page = uint16_t(latched & ~(kPageBytes - 1));
    for (uint32_t k = 0; k < kPageBytes; k++) {
        const uint16_t a = uint16_t(page + k);
        if (array_of(a) == array) {
            m->mem.bytes[a] = 0xFF;
        }
    }
    m->flash_changed(page, kPageBytes);
    SPDLOG_DEBUG("flash: page erase ${:04x}", page);
}

void FlashController::array_write(uint16_t addr, uint8_t value) {
    const std::optional<size_t> found = array_of(addr);
    if (!found) {
        return;
    }
    const size_t array = *found;
    const uint8_t cr = cr_[array];

    if (cr & (kPgm | kErase)) {
        if (latch_[array] < 0) {
            latch_[array] = addr;  // the first write latches the row / page
        }
        if ((cr & kPgm) && (cr & kHven)) {
            m->mem.bytes[addr] = uint8_t(m->mem.bytes[addr] & value);
            programmed_++;
            m->flash_changed(addr, 1);
        }
        return;
    }

    model_gap::warn(stray_write, array, "flash: write ${:02x} to ${:04x} with FL{}CR idle (ignored)", value,
                    addr, array + 1);
}

void FlashController::save(std::vector<uint8_t>& out) const {
    put_value(out, cr_);
    put_value(out, latch_);
    put_value(out, programmed_);
    put_value(out, erases_);
}

void FlashController::load(const uint8_t*& in) {
    get_value(in, cr_);
    get_value(in, latch_);
    get_value(in, programmed_);
    get_value(in, erases_);
}

}  // namespace sim::sys
