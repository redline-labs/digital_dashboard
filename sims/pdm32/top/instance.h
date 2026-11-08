// One simulated PDM32 with what is wired to it: the board (GZ60 + firmware + flash images), the plant harness, the
// stimulus, an outside CAN node (a tester / bridge) with the decoded data stream, the symbols and the DBC.
//. SimSession drives one; the scenario runner and the tests do too.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "pdm32/board/pdm32.h"
#include "pdm32/config/image_set.h"
#include "pdm32/plant/loads.h"
#include "hc08/engines.h"
#include "dbc_parser/ast.h"
#include "pdm32/top/stimulus.h"
#include "pdm32/top/symbols.h"

namespace sim::sys {

struct InstanceConfig {
    std::string firmware_path;  // S-records; default: $PDM_FIRMWARE_HEX, else the build's PDM_FIRMWARE_HEX
    pdm::config::ImageSet images;
    CpuEngine engine = kDefaultCpuEngine;
    std::string symbols_path;  // default: firmware/pdm_v191.symbols.json in the repository
    std::string dbc_path;      // default: dbcs/motec/PDM_Generic_Output.dbc
    bool cop_enabled = true;
};

// The paths the defaults resolve to.
std::string default_firmware_path();
std::string default_symbols_path();
std::string default_dbc_path();

// The outside world's CAN interface on the PDM's bus: logs every frame, keeps the latest decoded value of every DBC
// signal, and can send frames to the PDM.
class ExternalCanNode final : public CanNode {
   public:
    struct Logged {
        uint64_t t;
        CanFrame frame;
        bool from_pdm;
    };

    static constexpr size_t kLogLimit = 200'000;

    const dbc_parser::Database* dbc = nullptr;
    uint64_t tick_hz = 32'000'000;
    std::function<void(const CanFrame&, uint64_t)> on_frame;  // a bridge to the world outside

    bool receive(const CanFrame& frame, const CanNode& from, uint64_t now) override;
    void send(CanBus& bus, const CanFrame& frame, uint64_t now);

    std::deque<Logged> log;
    uint64_t logged_total = 0;  // frames ever logged (the log's first entry is number logged_total - log.size())

    struct Value {
        double value;
        uint64_t t;
    };
    std::map<std::string, Value> signals;
};

class Instance {
   public:
    explicit Instance(InstanceConfig config);
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    InstanceConfig cfg;
    Pdm32Board board;
    plant::Harness harness;
    std::shared_ptr<Stimulus> stimulus;
    ExternalCanNode can_node;
    Symbols symbols;
    dbc_parser::Database dbc;

    Machine& m() {
        return board.soc.m;
    }

    double seconds() {
        return m().seconds();
    }
    uint64_t ticks(double seconds) {
        return uint64_t(seconds * double(m().cfg.tick_hz) + 0.5);
    }

    // Power-on: RAM cleared to the pattern a cold GZ60 shows (zeros here), the boot stand-in's reset.
    void power_on();

    // Runs until `seconds` of sim time.
    Stop run_until_seconds(double seconds);
};

}  // namespace sim::sys
