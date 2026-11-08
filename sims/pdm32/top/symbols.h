// The firmware's symbols (firmware/pdm_v191.symbols.json): names to addresses and
// sizes for the debugger, the agent's reads and the scenarios' expectations.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sim::sys {

struct Symbol {
    std::string name;
    uint16_t address = 0;
    uint16_t size = 1;
    std::string kind;  // function, data, register, table, config, factory, cal
    std::string comment;
};

class Symbols {
   public:
    // Throws std::runtime_error naming the file on a read or parse error.
    static Symbols load(const std::string& path);

    const Symbol* find(std::string_view name) const;
    // the function containing `pc` (the last function at or below it, within its size)
    const Symbol* function_at(uint16_t pc) const;
    // "Name+0x12" for an address in a known symbol, else "$xxxx"
    std::string describe(uint16_t addr) const;
    std::vector<const Symbol*> search(std::string_view filter, size_t limit) const;

    size_t size() const {
        return symbols_.size();
    }
    bool empty() const {
        return symbols_.empty();
    }

   private:
    std::vector<Symbol> symbols_;  // sorted by address
};

}  // namespace sim::sys
