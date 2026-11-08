// The PDM32 board: the wiring tables (against the image when it is there), the output switch's behaviour into a load,
// an open circuit and a short, and the sense chain from a connector voltage to the firmware's 8-bit reading.
#include <cmath>

#include "pdm32/board/hi_side_switch.h"
#include "pdm32/board/pdm32.h"
#include "pdm32/board/wiring.h"
#include "hc08/image.h"
#include "firmware_path.h"
#include "mctest/test.h"

using namespace sim;
using namespace sim::sys;

TEST(board_scan_table_names_every_signal_once) {
    int out_v = 0;
    int out_i = 0;
    int inputs = 0;
    for (int ch = 0; ch < 32; ch++) {
        for (uint8_t mux = 0; mux < 32; mux++) {
            if ((mux & ~0x13) != 0) {
                continue;
            }
            switch (pdm32::sense_source(ch, mux).kind) {
                case pdm32::Sense::OutputVoltage:
                    out_v++;
                    break;
                case pdm32::Sense::OutputCurrent:
                    out_i++;
                    break;
                case pdm32::Sense::InputVoltage:
                    inputs++;
                    break;
                case pdm32::Sense::None:
                case pdm32::Sense::Battery:
                case pdm32::Sense::Rail9V5:
                case pdm32::Sense::BoardTemp:
                    break;
            }
        }
    }
    CHECK_EQ(out_v, 32);
    CHECK_EQ(out_i, 32);
    CHECK_EQ(inputs, 23);
    CHECK(pdm32::sense_source(7, 0x13).kind == pdm32::Sense::Rail9V5);
    CHECK(pdm32::sense_source(3, 0x00).kind == pdm32::Sense::BoardTemp);
}

TEST(board_wiring_matches_the_firmware_image) {
    const std::string hex = sim::test::firmware_hex();
    if (hex.empty()) {
        mctest::skip("no firmware image");
        return;
    }
    MemoryMap mem;
    mem.add_flash(0, 0x10000);
    load_image(mem, load_srec_file(hex));
    for (size_t k = 0; k < pdm32::kAdcScanTable.size(); k++) {
        mctest::check(mem.bytes[0xC4C5 + k] == pdm32::kAdcScanTable[k], "scan table byte " + std::to_string(k));
    }
    // the pin map: {shadow, mask}, the shadows naming PTF, PTE, PTD, PTC, PTA in turn
    const Port shadow_port[] = {Port::F, Port::E, Port::D, Port::C, Port::A};
    for (size_t k = 0; k < 32; k++) {
        const uint8_t shadow = mem.bytes[0xCB23 + 2 * k];
        const uint8_t mask = mem.bytes[0xCB24 + 2 * k];
        const pdm32::Pin pin = pdm32::kOutputPins[k];
        mctest::check(shadow >= 0xE9 && shadow <= 0xED && shadow_port[shadow - 0xE9] == pin.port,
                      "output " + std::to_string(k + 1) + " port");
        mctest::check(mask == (1u << pin.bit), "output " + std::to_string(k + 1) + " bit");
    }
}

TEST(switch_drives_a_resistive_load) {
    HiSideSwitch sw(SwitchParams::low_current());
    sw.step(0.001, true, 13.5, 6.0);
    CHECK(std::fabs(sw.current() - 13.5 / 6.02) < 0.01);
    CHECK(sw.output_voltage() > 13.4);
    CHECK(!sw.current_limited());
    sw.step(0.001, false, 13.5, 6.0);
    CHECK_EQ(sw.current(), 0.0);
    CHECK_EQ(sw.output_voltage(), 0.0);
}

TEST(switch_open_load_follows_the_supply) {
    HiSideSwitch sw;
    sw.step(0.001, true, 12.0, HiSideSwitch::kOpen);
    CHECK_EQ(sw.output_voltage(), 12.0);
    CHECK_EQ(sw.current(), 0.0);
}

TEST(switch_short_limits_overheats_and_cycles) {
    HiSideSwitch sw(SwitchParams::low_current());
    sw.step(0, true, 13.5, 0.005);
    CHECK(sw.current_limited());
    CHECK_EQ(sw.current(), sw.params().current_limit);
    CHECK(sw.output_voltage() < 1.0);
    double t = 0;
    while (!sw.thermal_shutdown() && t < 0.1) {
        sw.step(1e-4, true, 13.5, 0.005);
        t += 1e-4;
    }
    CHECK(sw.thermal_shutdown());
    CHECK(t < 0.01);  // milliseconds into a short
    CHECK_EQ(sw.output_voltage(), 0.0);
    for (int k = 0; k < 2000; k++) {
        sw.step(1e-4, true, 13.5, 0.005);
    }
    CHECK(sw.shutdowns() > 1);  // it restarts below the hysteresis and trips again
}

TEST(board_input_reading_through_the_adc) {
    Pdm32Board board;
    board.soc.cgm->set_bootloader_state();
    board.set_input(0, 12.0);
    // input 1 is the scan's first input entry: AD6 with PTB = 0x11
    double pin = 0;
    for (int ch = 0; ch < 32; ch++) {
        for (uint8_t mux = 0; mux < 0x14; mux++) {
            const auto s = pdm32::sense_source(ch, mux);
            if (s.kind == pdm32::Sense::InputVoltage && s.index == 0) {
                pin = board.adc_pin(ch, mux);
            }
        }
    }
    const int code8 = int(std::lround(pin / 5.0 * 1023.0)) >> 2;
    CHECK_EQ(code8, 60);  // 12 V at 0.2 V/bit
    board.set_input_open(0);
    CHECK_EQ(board.input_voltage(0), board.battery());  // the pull-up
}

TEST(board_temperature_reading_lands_in_the_firmware_table) {
    // the firmware indexes its temperature table with 4 x the raw count (0 degC at index 87, ~0.82 degC per index),
    // so room temperature is a raw count near 29; a unit at about 19 degC read 27
    Pdm32Board board;
    board.soc.cgm->set_bootloader_state();
    board.set_board_temperature(25.0);
    double pin = 0;
    for (int ch = 0; ch < 32; ch++) {
        for (uint8_t mux = 0; mux < 0x14; mux++) {
            if (pdm32::sense_source(ch, mux).kind == pdm32::Sense::BoardTemp) {
                pin = board.adc_pin(ch, mux);
            }
        }
    }
    const int code8 = int(std::lround(pin / 5.0 * 1023.0)) >> 2;
    CHECK_EQ(code8, 29);
    const double celsius = (4.0 * code8 - 87.3) * 0.8203;
    CHECK(celsius > 20.0 && celsius < 30.0);
}
