// cal-header against the header sims/pdm32 compiles in: the compiled-in calibration, encoded back into a block,
// must generate that very file. So the generator, the encoder and the committed table agree byte for byte, and a
// fresh dump turned into a header is a one-line diff in review. Plus blocks it must refuse.
#include <fstream>
#include <sstream>

#include "mctest/test.h"
#include "pdm32/config/current_cal.h"
#include "pdm32/config/current_cal_dumped.h"
#include "pdm_flash_dump/cal_header.h"

TEST(cal_header_regenerates_the_committed_table) {
    const std::vector<uint8_t> block = pdm::config::encode_current_cal(pdm::config::kCurrentCalDumped);
    const std::string generated = pdm_flash_dump::current_cal_header(block, "current_cal.bin", "dumped");
    std::ifstream in(CURRENT_CAL_HEADER);
    std::stringstream committed;
    committed << in.rdbuf();
    mctest::check(generated == committed.str(), "the generated header is the committed one");
    CHECK_EQ(uint16_t((block[block.size() - 2] << 8) | block.back()), pdm::config::kCurrentCalDumpedCrc);
}

TEST(cal_header_refuses_bad_blocks) {
    std::vector<uint8_t> block = pdm::config::encode_current_cal(pdm::config::kCurrentCalDumped);
    const auto refuses = [](const std::vector<uint8_t>& b) {
        try {
            (void)pdm_flash_dump::current_cal_header(b, "x", "dumped");
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    std::vector<uint8_t> crc = block;
    crc[100] ^= 1;
    CHECK(refuses(crc));
    CHECK(refuses(std::vector<uint8_t>(block.begin(), block.begin() + 100)));
    CHECK(refuses({}));
    std::vector<uint8_t> format = block;
    format[1] = 9;
    CHECK(refuses(format));
}
