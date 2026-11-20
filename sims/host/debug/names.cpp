#include "host/debug/names.h"

#include <bit>
#include <cstring>
#include <regex>

#include "host/control.h"

namespace sim::sys {

namespace {

const std::regex kName(R"(^([A-Za-z_]\w*|\$[0-9A-Fa-f]+|0[xX][0-9A-Fa-f]+)(?:\[(\d+)\])?(?:\.([wlf]))?$)");

std::optional<uint64_t> parse_address(const std::string& text) {
    try {
        if (text.size() > 1 && text[0] == '$') {
            return std::stoull(text.substr(1), nullptr, 16);
        }
        if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
            return std::stoull(text.substr(2), nullptr, 16);
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

}  // namespace

std::optional<MemoryName> memory_name(const DebugTarget& target, const std::string& name) {
    std::smatch m;
    if (!std::regex_match(name, m, kName)) {
        return std::nullopt;
    }
    MemoryName out;
    if (const auto a = parse_address(m[1].str())) {
        out.address = *a;
    } else if (const Symbol* s = target.symbols().find(m[1].str())) {
        out.address = s->address;
    } else {
        return std::nullopt;
    }
    if (m[2].matched) {
        try {
            out.address += std::stoull(m[2].str());
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    if (m[3].matched) {
        const char w = m[3].str()[0];
        out.size = w == 'w' ? 2 : 4;
        out.real = w == 'f';
    }
    return out;
}

std::optional<double> read_name(DebugTarget& target, const std::string& name, std::string& error) {
    if (name == "t") {
        return target.seconds();
    }
    if (const auto r = target.read_register(name)) {
        return double(*r);
    }
    if (const auto v = target.read_device_name(name, error)) {
        return v;
    }
    if (!error.empty()) {
        return std::nullopt;
    }
    if (const auto mem = memory_name(target, name)) {
        uint8_t bytes[4] = {};
        if (!target.peek(mem->address, bytes, mem->size)) {
            error = name + ": nothing at that address";
            return std::nullopt;
        }
        uint32_t v = 0;
        for (unsigned k = 0; k < mem->size; ++k) {
            v = (v << 8) | bytes[k];
        }
        return mem->real ? double(std::bit_cast<float>(v)) : double(v);
    }
    error = name + ": unknown name";
    return std::nullopt;
}

std::string set_name(DebugTarget& target, const std::string& name, const nlohmann::json& value, bool allow_write) {
    if (const auto done = target.write_device_name(name, value)) {
        return *done;
    }
    if (target.read_register(name)) {
        if (!allow_write) {
            return name + ": register writes need --allow-write";
        }
        const auto v = parse_number(value);
        if (!v || *v < 0) {
            return name + ": expected a number";
        }
        return target.write_register(name, uint64_t(*v)) ? "" : name + ": not writable";
    }
    if (const auto mem = memory_name(target, name)) {
        if (!allow_write) {
            return name + ": memory writes need --allow-write";
        }
        const auto v = parse_number(value);
        if (!v) {
            return name + ": expected a number";
        }
        uint32_t bits;
        if (mem->real) {
            bits = std::bit_cast<uint32_t>(float(*v));
        } else {
            const double limit = mem->size == 1 ? 255.0 : mem->size == 2 ? 65535.0 : 4294967295.0;
            if (*v < 0 || *v > limit) {
                return name + ": out of range for " + std::to_string(mem->size) + " byte(s)";
            }
            bits = uint32_t(*v);
        }
        uint8_t bytes[4];
        for (unsigned k = 0; k < mem->size; ++k) {
            bytes[k] = uint8_t(bits >> (8 * (mem->size - 1 - k)));
        }
        return target.poke(mem->address, bytes, mem->size) ? "" : name + ": not writable";
    }
    return name + ": unknown name";
}

}  // namespace sim::sys
