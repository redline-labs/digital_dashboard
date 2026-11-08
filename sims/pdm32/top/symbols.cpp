#include "pdm32/top/symbols.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace sim::sys {

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
        sym.address = uint16_t(e.at("address").get<int>());
        sym.size = uint16_t(std::max(1, e.value("size", 1)));
        sym.kind = e.value("kind", "data");
        sym.comment = e.value("comment", "");
        s.symbols_.push_back(std::move(sym));
    }
    std::stable_sort(s.symbols_.begin(), s.symbols_.end(),
                     [](const Symbol& a, const Symbol& b) { return a.address < b.address; });
    return s;
}

const Symbol* Symbols::find(std::string_view name) const {
    for (const Symbol& s : symbols_) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

const Symbol* Symbols::function_at(uint16_t pc) const {
    const Symbol* best = nullptr;
    for (const Symbol& s : symbols_) {
        if (s.kind != "function" || s.address > pc) {
            continue;
        }
        if (pc < s.address + s.size && (!best || s.address >= best->address)) {
            best = &s;
        }
    }
    return best;
}

std::string Symbols::describe(uint16_t addr) const {
    char text[64];
    if (const Symbol* f = function_at(addr)) {
        if (addr == f->address) {
            return f->name;
        }
        std::snprintf(text, sizeof(text), "%s+0x%x", f->name.c_str(), unsigned(addr - f->address));
        return text;
    }
    for (const Symbol& s : symbols_) {
        if (s.kind != "function" && addr >= s.address && addr < s.address + s.size) {
            if (addr == s.address) {
                return s.name;
            }
            std::snprintf(text, sizeof(text), "%s+%u", s.name.c_str(), unsigned(addr - s.address));
            return text;
        }
    }
    std::snprintf(text, sizeof(text), "$%04x", addr);
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
