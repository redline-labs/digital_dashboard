#include "host/debug/symbols.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace sim::sys {

namespace {

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("symbols: cannot open " + path);
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

// An ELF file's integer fields in its own byte order and width.
class ElfReader {
   public:
    ElfReader(const std::vector<uint8_t>& bytes, const std::string& path) : b_(bytes), path_(path) {
        if (b_.size() < 52 || std::memcmp(b_.data(), "\x7f" "ELF", 4) != 0) {
            fail("not an ELF file");
        }
        wide_ = b_[4] == 2;
        big_ = b_[5] == 2;
    }
    bool wide() const {
        return wide_;
    }
    uint64_t u(size_t at, size_t n) const {
        if (at + n > b_.size()) {
            fail("truncated");
        }
        uint64_t v = 0;
        for (size_t i = 0; i < n; ++i) {
            const uint64_t byte = b_[at + i];
            v |= big_ ? byte << (8 * (n - 1 - i)) : byte << (8 * i);
        }
        return v;
    }
    uint64_t addr(size_t at) const {
        return u(at, wide_ ? 8 : 4);
    }
    std::string str(size_t at) const {
        std::string out;
        while (at < b_.size() && b_[at]) {
            out += char(b_[at++]);
        }
        return out;
    }
    [[noreturn]] void fail(const std::string& why) const {
        throw std::runtime_error("symbols: " + path_ + ": " + why);
    }

   private:
    const std::vector<uint8_t>& b_;
    std::string path_;
    bool wide_ = false;
    bool big_ = false;
};

}  // namespace

Symbols Symbols::load(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("symbols: cannot open " + path);
    }
    nlohmann::json j;
    try {
        file >> j;
    } catch (const std::exception& e) {
        throw std::runtime_error("symbols: " + path + ": " + e.what());
    }
    Symbols s;
    for (const auto& e : j.at("symbols")) {
        Symbol sym;
        sym.name = e.at("name").get<std::string>();
        sym.address = e.at("address").get<uint64_t>();
        sym.size = uint32_t(std::max<int64_t>(1, e.value("size", int64_t{1})));
        sym.kind = e.value("kind", "data");
        sym.comment = e.value("comment", "");
        s.symbols_.push_back(std::move(sym));
    }
    s.sort();
    return s;
}

Symbols Symbols::load_elf(const std::string& path) {
    const std::vector<uint8_t> bytes = read_file(path);
    const ElfReader r(bytes, path);
    const bool wide = r.wide();
    const uint64_t shoff = wide ? r.u(0x28, 8) : r.u(0x20, 4);
    const uint64_t shentsize = r.u(wide ? 0x3A : 0x2E, 2);
    const uint64_t shnum = r.u(wide ? 0x3C : 0x30, 2);
    Symbols s;
    for (uint64_t i = 0; i < shnum; ++i) {
        const size_t sh = size_t(shoff + i * shentsize);
        if (r.u(sh + 4, 4) != 2) {  // SHT_SYMTAB
            continue;
        }
        const uint64_t offset = wide ? r.u(sh + 0x18, 8) : r.u(sh + 0x10, 4);
        const uint64_t size = wide ? r.u(sh + 0x20, 8) : r.u(sh + 0x14, 4);
        const uint64_t link = r.u(sh + (wide ? 0x28 : 0x18), 4);
        const uint64_t entsize = wide ? r.u(sh + 0x38, 8) : r.u(sh + 0x24, 4);
        const size_t strsh = size_t(shoff + link * shentsize);
        const uint64_t stroff = wide ? r.u(strsh + 0x18, 8) : r.u(strsh + 0x10, 4);
        if (entsize == 0) {
            r.fail("symbol table without an entry size");
        }
        for (uint64_t at = offset; at + entsize <= offset + size; at += entsize) {
            const size_t e = size_t(at);
            const uint64_t name = r.u(e, 4);
            uint64_t value, sz;
            unsigned info, shndx;
            if (wide) {
                info = unsigned(r.u(e + 4, 1));
                shndx = unsigned(r.u(e + 6, 2));
                value = r.u(e + 8, 8);
                sz = r.u(e + 16, 8);
            } else {
                value = r.u(e + 4, 4);
                sz = r.u(e + 8, 4);
                info = unsigned(r.u(e + 12, 1));
                shndx = unsigned(r.u(e + 14, 2));
            }
            const unsigned type = info & 0xF;
            if ((type != 1 && type != 2) || shndx == 0 || name == 0) {  // STT_OBJECT, STT_FUNC, defined, named
                continue;
            }
            Symbol sym;
            sym.name = r.str(size_t(stroff + name));
            sym.address = value;
            sym.size = uint32_t(std::max<uint64_t>(1, sz));
            sym.kind = type == 2 ? "function" : "data";
            s.symbols_.push_back(std::move(sym));
        }
    }
    s.sort();
    return s;
}

Symbols Symbols::load_any(const std::string& path) {
    const bool json = path.size() >= 5 && path.compare(path.size() - 5, 5, ".json") == 0;
    return json ? load(path) : load_elf(path);
}

void Symbols::merge(const Symbols& other) {
    symbols_.insert(symbols_.end(), other.symbols_.begin(), other.symbols_.end());
    sort();
}

void Symbols::sort() {
    std::stable_sort(symbols_.begin(), symbols_.end(),
                     [](const Symbol& a, const Symbol& b) { return a.address < b.address; });
}

const Symbol* Symbols::find(std::string_view name) const {
    for (const Symbol& s : symbols_) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

const Symbol* Symbols::function_at(uint64_t pc) const {
    const Symbol* best = nullptr;
    for (const Symbol& s : symbols_) {
        if (s.address > pc) {
            break;  // sorted by address
        }
        if (s.kind == "function" && pc < s.address + s.size && (!best || s.address >= best->address)) {
            best = &s;
        }
    }
    return best;
}

std::string Symbols::describe(uint64_t addr, int digits) const {
    char text[160];
    if (const Symbol* f = function_at(addr)) {
        if (addr == f->address) {
            return f->name;
        }
        std::snprintf(text, sizeof(text), "%s+0x%llx", f->name.c_str(),
                      static_cast<unsigned long long>(addr - f->address));
        return text;
    }
    for (const Symbol& s : symbols_) {
        if (s.kind != "function" && addr >= s.address && addr < s.address + s.size) {
            if (addr == s.address) {
                return s.name;
            }
            std::snprintf(text, sizeof(text), "%s+%llu", s.name.c_str(),
                          static_cast<unsigned long long>(addr - s.address));
            return text;
        }
    }
    std::snprintf(text, sizeof(text), "$%0*llx", digits, static_cast<unsigned long long>(addr));
    return text;
}

std::vector<const Symbol*> Symbols::search(std::string_view filter, size_t limit) const {
    const auto lower = [](std::string_view in) {
        std::string out(in);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return out;
    };
    const std::string needle = lower(filter);
    std::vector<const Symbol*> found;
    for (const Symbol& s : symbols_) {
        if (found.size() >= limit) {
            break;
        }
        if (needle.empty() || lower(s.name).find(needle) != std::string::npos) {
            found.push_back(&s);
        }
    }
    return found;
}

}  // namespace sim::sys
