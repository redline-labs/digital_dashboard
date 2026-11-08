#include "pdm32/top/instance.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "core/core.h"
#include "dbc_parser/dbc_parser.h"
#include "dbc_parser/decode.h"

#include "pdm32/board/boot_hle.h"
#include "pdm32/config/current_cal.h"
#include "hc08/image.h"

namespace sim::sys {

std::string default_firmware_path() {
    if (const char* env = std::getenv("PDM_FIRMWARE_HEX"); env && *env) {
        return env;
    }
    if (const char* home = std::getenv("HOME")) {
        return std::string(home) + "/src/motec/MoTeC/PDM Manager/1.9/pdm.hex";
    }
    return "pdm.hex";
}

std::string default_symbols_path() {
    return core::paths::resource("sims/pdm32/firmware/pdm_v191.symbols.json");
}

std::string default_dbc_path() {
    return core::paths::resource("dbcs/motec/PDM_Generic_Output.dbc");
}

namespace {

// The data stream's DBC, through the tree's own parser. A file it cannot read leaves the stream undecoded, said once.
dbc_parser::Database load_dbc(const std::string& path) {
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    const std::string contents = text.str();
    dbc_parser::Parser parser(contents);
    if (auto database = parser.parse()) {
        return std::move(*database);
    }
    SPDLOG_WARN("dbc: {} could not be parsed: the data stream is not decoded", path);
    return {};
}

}  // namespace

bool ExternalCanNode::receive(const CanFrame& frame, const CanNode& from, uint64_t now) {
    (void)from;
    log.push_back({now, frame, true});
    logged_total++;
    if (log.size() > kLogLimit) {
        log.pop_front();
    }
    if (dbc) {
        if (const dbc_parser::Message* message = dbc_parser::findMessage(*dbc, frame.id, frame.isExtended)) {
            const auto payload = std::span<const uint8_t>(frame.data.data(), classic_payload_bytes(frame));
            for (const auto& [name, value] : dbc_parser::decodeMessage(*message, payload)) {
                signals[name] = Value{value, now};
            }
        }
    }
    if (on_frame) {
        on_frame(frame, now);
    }
    return true;  // the tester acknowledges every frame
}

void ExternalCanNode::send(CanBus& bus, const CanFrame& frame, uint64_t now) {
    log.push_back({now, frame, false});
    logged_total++;
    if (log.size() > kLogLimit) {
        log.pop_front();
    }
    bus.send(*this, frame, now);
}

Instance::Instance(InstanceConfig config) : cfg(std::move(config)), board(MachineConfig{.engine = cfg.engine}) {
    if (cfg.firmware_path.empty()) {
        cfg.firmware_path = default_firmware_path();
    }
    if (cfg.symbols_path.empty()) {
        cfg.symbols_path = default_symbols_path();
    }
    if (cfg.dbc_path.empty()) {
        cfg.dbc_path = default_dbc_path();
    }

    // the application image, then the flash blocks the bootloader/factory/PDM Manager would have written
    const Image app = load_srec_file(cfg.firmware_path);
    load_image(m().mem, app);
    SPDLOG_INFO("firmware: {} ({}, ${:04x}-${:04x})", cfg.firmware_path, app.name, app.lowest(), app.highest());
    for (const pdm::config::FlashBlock& block : cfg.images.blocks) {
        for (size_t k = 0; k < block.bytes.size(); k++) {
            m().mem.bytes[uint16_t(block.address + k)] = block.bytes[k];
        }
        SPDLOG_INFO("flash: {} at ${:04x}, {} bytes", block.name, block.address, block.bytes.size());
    }
    // the board's current sense matches the calibration in flash, so the firmware reports the true current
    if (const pdm::config::FlashBlock* cal_block = cfg.images.find("current_cal")) {
        try {
            const pdm::config::CurrentCal cal = pdm::config::decode_current_cal(cal_block->bytes);
            for (size_t k = 0; k < Pdm32Board::kOutputs; k++) {
                board.set_current_sense(k, cal.outputs[k].raw_for_count());
            }
        } catch (const std::runtime_error& e) {
            SPDLOG_WARN("{}: the current sense stays uncalibrated", e.what());
        }
    }
    install_boot_stand_in(board);

    stimulus = std::make_shared<Stimulus>(board, harness);
    m().add(stimulus);

    if (std::filesystem::exists(cfg.symbols_path)) {
        symbols = Symbols::load(cfg.symbols_path);
    } else {
        SPDLOG_WARN("symbols: {} not found: names are unavailable", cfg.symbols_path);
    }
    if (std::filesystem::exists(cfg.dbc_path)) {
        dbc = load_dbc(cfg.dbc_path);
    } else {
        SPDLOG_WARN("dbc: {} not found: the data stream is not decoded", cfg.dbc_path);
    }

    can_node.dbc = &dbc;
    can_node.tick_hz = m().cfg.tick_hz;
    board.can.attach(can_node);

    board.soc.cop->set_enabled(cfg.cop_enabled);
    power_on();
}

void Instance::power_on() {
    m().reset(ResetCause::PowerOn);
    board.soc.cop->set_enabled(cfg.cop_enabled);
    stimulus->apply();
}

Stop Instance::run_until_seconds(double seconds) {
    return m().run_until(ticks(seconds));
}

}  // namespace sim::sys
