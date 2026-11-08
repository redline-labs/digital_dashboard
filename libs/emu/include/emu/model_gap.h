// Model gaps: the firmware exercises a device register, command or case the simulator does not model (or models only
// as storage) and whose function is not understood. Each one is logged once, as a warning prefixed "model gap:", so a
// run shows what it touched; the warnings are the list.
//
// After its first warning a site costs one relaxed atomic load, so a gap on a path the firmware hammers does not slow
// the run. The flags are per process: a long-running host clears them with reset_all, and a batch run keeps them, so a
// gap warns once however many cases it runs.
//
//   static sim::model_gap::Once<1> start_up_write;
//   sim::model_gap::warn(start_up_write, "asic: write 0x{:03x} to register 0x240, function unknown", value);
//
//   sim::model_gap::Once<128> unknown_register;  // one flag per key 0..N-1, at namespace scope or static
//   sim::model_gap::warn(unknown_register, idx, "asic: register 0x{:03x} read, not modelled", idx << 3);
//
// The arguments are evaluated on every call (pass values that are already at hand); the message is formatted only
// when it is logged. The log line carries the caller's file and line.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <source_location>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

namespace sim::model_gap {

class Site {
   public:
    virtual void reset() = 0;

   protected:
    Site();
    ~Site() = default;
};

namespace detail {

struct Registry {
    std::mutex mutex;
    std::vector<Site*> sites;
};

inline Registry& registry() {
    static Registry instance;
    return instance;
}

}  // namespace detail

inline Site::Site() {
    detail::Registry& reg = detail::registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    reg.sites.push_back(this);
}

// Every site warns again (a new Run in sim-gui).
inline void reset_all() {
    detail::Registry& reg = detail::registry();
    std::lock_guard<std::mutex> lock(reg.mutex);

    for (Site* site : reg.sites) {
        site->reset();
    }
}

// One flag per key 0..N-1; a key out of range shares the last flag.
template <size_t N>
class Once final : public Site {
   public:
    // true the first time `key` is seen (since the last reset_all)
    bool first(size_t key) {
        std::atomic<bool>& flag = seen_[key < N ? key : N - 1];
        return !flag.load(std::memory_order_relaxed) && !flag.exchange(true, std::memory_order_relaxed);
    }

    void reset() override {
        for (std::atomic<bool>& flag : seen_) {
            flag.store(false, std::memory_order_relaxed);
        }
    }

   private:
    std::array<std::atomic<bool>, N> seen_{};
};

// A format string with the caller's source location, checked at compile time against the arguments.
template <typename... Args>
struct Format {
    template <typename Text>
        requires std::is_convertible_v<const Text&, std::string_view>
    consteval Format(const Text& text, std::source_location where = std::source_location::current())
        : text(text), where(where) {
    }

    fmt::format_string<Args...> text;
    std::source_location where;
};

// Logs "model gap: <message>" as a warning the first time `key` is seen at `site`.
template <size_t N, typename... Args>
void warn(Once<N>& site, size_t key, Format<std::type_identity_t<Args>...> format, Args&&... args) {
    if (!site.first(key)) {
        return;
    }

    const std::string message = fmt::format(format.text, std::forward<Args>(args)...);
    const spdlog::source_loc where{format.where.file_name(), int(format.where.line()), format.where.function_name()};
    spdlog::default_logger_raw()->log(where, spdlog::level::warn, "model gap: {}", message);
}

// A site with one flag: logs the first time it is reached.
template <typename... Args>
void warn(Once<1>& site, Format<std::type_identity_t<Args>...> format, Args&&... args) {
    warn(site, 0, format, std::forward<Args>(args)...);
}

}  // namespace sim::model_gap
