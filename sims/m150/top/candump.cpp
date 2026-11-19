#include "m150/top/candump.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace sim::m150 {

std::string format_candump(const sys::CanFrame& f, int bus, uint64_t now, uint64_t hz) {
    char buf[96];
    int n = std::snprintf(buf, sizeof buf, "(%.6f) can%d %0*X#", double(now) / double(hz), bus, f.isExtended ? 8 : 3,
                          f.id);
    if (f.isRTR) {
        n += std::snprintf(buf + n, sizeof buf - size_t(n), "R%u", f.len);
    } else {
        for (unsigned k = 0; k < f.len && k < 8; ++k) {
            n += std::snprintf(buf + n, sizeof buf - size_t(n), "%02X", f.data[k]);
        }
    }
    return std::string(buf, size_t(n));
}

std::vector<CanInjector::Entry> parse_candump(const std::string& text, uint64_t hz, double start_s) {
    std::istringstream in(text);
    std::vector<CanInjector::Entry> out;
    std::string line;
    double first = -1;
    while (std::getline(in, line)) {
        double t = 0;
        char iface[32] = {};
        char frame[64] = {};
        if (std::sscanf(line.c_str(), " (%lf) %31s %63s", &t, iface, frame) != 3) {
            continue;
        }
        const char* hash = std::strchr(frame, '#');
        if (!hash) {
            continue;
        }
        if (first < 0) {
            first = t;
        }
        CanInjector::Entry e{};
        e.bus = std::clamp(int(std::strtol(iface + std::strcspn(iface, "0123456789"), nullptr, 10)), 0, 2);
        const std::string id(frame, size_t(hash - frame));
        e.frame.id = uint32_t(std::strtoul(id.c_str(), nullptr, 16));
        e.frame.isExtended = id.size() > 3;
        const std::string data(hash + 1);
        e.frame.len = uint8_t(std::min<size_t>(data.size() / 2, 8));
        for (size_t k = 0; k < e.frame.len; ++k) {
            e.frame.data[k] = uint8_t(std::strtoul(data.substr(2 * k, 2).c_str(), nullptr, 16));
        }
        e.at = uint64_t((t - first + start_s) * double(hz));
        out.push_back(e);
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.at < b.at; });
    return out;
}

std::vector<CanInjector::Entry> read_candump(const std::string& path, uint64_t hz, double start_s) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot read " + path);
    }
    std::stringstream text;
    text << in.rdbuf();
    return parse_candump(text.str(), hz, start_s);
}

}  // namespace sim::m150
