// The simulated M150 as host/server.h serves it: the board, its debug target and session (host/debug), and what
// `m150_sim serve`'s config may say: three CAN ports, CAN1 (FlexCAN A) on can0 by default and CAN2/CAN3 (FlexCAN B,
// C) off until named; the image, its symbol files and the engine.
//
// Which FlexCAN drives which connector pair is not known; A, B, C are taken as CAN1, CAN2, CAN3.
#pragma once

#include <array>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "host/debug/session.h"
#include "host/serve_config.h"
#include "host/served_device.h"
#include "m150/board/m150.h"
#include "m150/top/candump.h"
#include "m150/top/target.h"

namespace sim::m150 {

const sys::ServeSchema& m150_serve_schema();

// "interp"/"interpreter", "block" or "jit"; nullopt otherwise.
std::optional<e200::Engine> parse_engine(const std::string& name);

struct M150Options {
    std::string image;
    std::vector<std::string> symbols;  // ELF files (the kernel's, the application's) or JSON tables
    e200::Engine engine = e200::Engine::Jit;
    bool allow_write = false;          // agents may write memory and registers
};

// Splits "a.elf,b.elf" into paths.
std::vector<std::string> split_paths(const std::string& list);

class M150Served final : public sys::ServedDevice {
   public:
    // Boots the image. Throws std::runtime_error when the image or a symbol file cannot be read, or the image holds
    // no reset configuration.
    explicit M150Served(M150Options options);

    Board& board() {
        return *board_;
    }
    sys::DebugSession& debug() {
        return *session_;
    }

    double seconds() const override;
    sys::RunOutcome run_until(double t_seconds) override;
    const std::vector<std::string>& can_ports() const override;
    void can_send(size_t port, const helpers::CanFrame& frame) override;
    void set_can_sink(size_t port, std::function<void(const helpers::CanFrame&, uint64_t sim_us)> sink) override;
    uint32_t can_bitrate(size_t port) const override;
    void register_methods(agent_control::Dispatcher& dispatcher, const sys::ServerHooks& hooks) override;
    nlohmann::json info() const override;

    nlohmann::json status();
    // the console's last lines (eSCI A)
    const std::deque<std::string>& console() const {
        return console_;
    }

    // Every frame on each port, either way.
    struct Logged {
        uint64_t t;
        size_t port;
        sys::CanFrame frame;
        bool from_ecu;
    };
    static constexpr size_t kLogLimit = 200'000;
    const std::deque<Logged>& can_log() const {
        return log_;
    }
    uint64_t can_logged_total() const {
        return logged_total_;
    }

   private:
    // The outside world on one bus: frames from it are sent from here, and every frame the board puts on the bus
    // reaches the sink and the log.
    class Port final : public sys::CanNode {
       public:
        M150Served* owner = nullptr;
        size_t index = 0;
        std::function<void(const helpers::CanFrame&, uint64_t sim_us)> sink;
        uint64_t ticks_per_us = 125;
        bool receive(const sys::CanFrame& frame, const sys::CanNode&, uint64_t now) override {
            owner->log(index, frame, now, true);
            if (sink) {
                sink(frame, now / ticks_per_us);
            }
            return true;
        }
    };

    void log(size_t port, const sys::CanFrame& frame, uint64_t now, bool from_ecu);

    M150Options options_;
    std::unique_ptr<Board> board_;
    std::unique_ptr<M150Target> target_;
    std::unique_ptr<sys::DebugSession> session_;
    std::array<Port, 3> ports_;
    std::deque<std::string> console_;
    std::string line_;
    std::deque<Logged> log_;
    uint64_t logged_total_ = 0;
};

}  // namespace sim::m150
