// A firmware's symbols: names to addresses and sizes, for the debugger, the agent's reads and a scenario's
// expectations. From a JSON table ({"symbols": [{name, address, size, kind, comment}]}, as the PDM's
// pdm_v191.symbols.json) or from an ELF file's symbol table (functions and objects), any number merged.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sim::sys {

struct Symbol {
    std::string name;
    uint64_t address = 0;
    uint32_t size = 1;
    std::string kind;  // function, data, register, table, config, factory, cal
    std::string comment;
};

class Symbols {
   public:
    // Throw std::runtime_error naming the file on a read or parse error.
    static Symbols load(const std::string& path);      // JSON
    static Symbols load_elf(const std::string& path);  // an ELF32/ELF64 file's .symtab
    // By extension: ".json" is a table, anything else an ELF file.
    static Symbols load_any(const std::string& path);

    // Adds another table's symbols (a kernel's and an application's).
    void merge(const Symbols& other);

    const Symbol* find(std::string_view name) const;
    // the function containing `pc` (the last function at or below it, within its size)
    const Symbol* function_at(uint64_t pc) const;
    // "Name+0x12" for an address in a known symbol, else the address in hex with `digits` digits ("$xxxx")
    std::string describe(uint64_t addr, int digits = 4) const;
    std::vector<const Symbol*> search(std::string_view filter, size_t limit) const;

    size_t size() const {
        return symbols_.size();
    }
    bool empty() const {
        return symbols_.empty();
    }

   private:
    void sort();

    std::vector<Symbol> symbols_;  // sorted by address
};

}  // namespace sim::sys
