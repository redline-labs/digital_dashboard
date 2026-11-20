// Register storage for MPC55xx modules: a byte array read and written big-endian at any size, and the stub module
// built on it -- a module the simulator does not model, whose registers are storage and whose first write of each
// register is logged as a model gap, so firmware that starts depending on one shows up.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "e200/machine.h"

namespace sim::mpc55xx {

class RegisterFile {
   public:
    explicit RegisterFile(size_t size) : bytes_(size, 0) {
    }

    size_t size() const {
        return bytes_.size();
    }
    bool contains(uint32_t offset, unsigned size) const {
        return offset + size <= bytes_.size();
    }
    uint32_t get(uint32_t offset, unsigned size = 4) const {
        uint32_t v = 0;
        for (unsigned i = 0; i < size; ++i) {
            v = (v << 8) | (offset + i < bytes_.size() ? bytes_[offset + i] : 0);
        }
        return v;
    }
    void set(uint32_t offset, unsigned size, uint32_t value) {
        for (unsigned i = 0; i < size; ++i) {
            if (offset + i < bytes_.size()) {
                bytes_[offset + i] = uint8_t(value >> (8 * (size - 1 - i)));
            }
        }
    }
    void set32(uint32_t offset, uint32_t value) {
        set(offset, 4, value);
    }
    uint32_t get32(uint32_t offset) const {
        return get(offset, 4);
    }
    void clear() {
        std::fill(bytes_.begin(), bytes_.end(), uint8_t{0});
    }
    void save(std::vector<uint8_t>& out) const {
        out.insert(out.end(), bytes_.begin(), bytes_.end());
    }
    void load(const uint8_t*& in) {
        std::copy(in, in + bytes_.size(), bytes_.begin());
        in += bytes_.size();
    }

   private:
    std::vector<uint8_t> bytes_;
};

// A module the simulator does not model: storage, with fixed values for registers whose reads must say something
// (a PLL's lock bit), and a model gap logged the first time each register is written.
class StubModule final : public e200::BusDevice {
   public:
    struct Fixed {
        uint32_t offset;
        uint32_t value;  // a 32-bit register that always reads this
    };

    StubModule(std::string name, uint32_t size, std::vector<Fixed> fixed = {})
        : BusDevice(std::move(name)), regs_(size), written_(size / 4, false), fixed_(std::move(fixed)) {
    }

    uint32_t read(uint32_t offset, unsigned size) override;
    void write(uint32_t offset, unsigned size, uint32_t value) override;
    void reset() override {
        regs_.clear();
    }
    void save(std::vector<uint8_t>& out) const override {
        regs_.save(out);
    }
    void load(const uint8_t*& in) override {
        regs_.load(in);
    }

   private:
    RegisterFile regs_;
    std::vector<bool> written_;
    std::vector<Fixed> fixed_;
};

}  // namespace sim::mpc55xx
