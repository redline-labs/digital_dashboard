// A small HC08 code emitter: the instructions a synthetic configuration's logic routines need, with forward labels for
// the branches. Not a general assembler.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace sim {

class Hc08Asm {
   public:
    struct Label {
        size_t id;
    };

    Label label() {
        positions_.push_back(std::nullopt);
        return Label{positions_.size() - 1};
    }

    void bind(Label l) {
        positions_[l.id] = code_.size();
    }

    void clr_dir(uint8_t addr) {
        emit({0x3F, addr});
    }
    void clra() {
        emit({0x4F});
    }
    void lda_imm(uint8_t v) {
        emit({0xA6, v});
    }
    void lda_ix2(uint16_t offset) {
        emit({0xD6, uint8_t(offset >> 8), uint8_t(offset)});
    }
    void ldhx_dir(uint8_t addr) {
        emit({0x55, addr});
    }
    void cmp_imm(uint8_t v) {
        emit({0xA1, v});
    }
    void sta_dir(uint8_t addr) {
        emit({0xB7, addr});
    }
    void rts() {
        emit({0x81});
    }
    void bhi(Label target) {
        branch(0x22, target);
    }
    void bls(Label target) {
        branch(0x23, target);
    }
    void bra(Label target) {
        branch(0x20, target);
    }

    // The code with every branch resolved. Throws std::logic_error for an unbound label or a branch out of reach.
    std::vector<uint8_t> finish() const;

   private:
    void emit(std::initializer_list<uint8_t> bytes) {
        code_.insert(code_.end(), bytes);
    }
    void branch(uint8_t opcode, Label target) {
        code_.push_back(opcode);
        fixups_.push_back({code_.size(), target.id});
        code_.push_back(0);
    }

    struct Fixup {
        size_t at;  // the offset byte's position
        size_t label;
    };
    std::vector<uint8_t> code_;
    std::vector<std::optional<size_t>> positions_;
    std::vector<Fixup> fixups_;
};

}  // namespace sim
