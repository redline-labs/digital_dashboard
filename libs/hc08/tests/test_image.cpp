// S-record parsing.
#include "hc08/image.h"
#include "mctest/test.h"

using namespace sim;

TEST(image_parses_s19_with_header_and_entry) {
    // S0 "HDR", one S1 of 4 bytes at $8000, S9 entry $8000
    const std::string text =
        "S00600004844521B\n"
        "S1078000B000B1E532\n"
        "S90380007C\n";
    const Image image = parse_srec(text);
    CHECK(image.name == "HDR");
    CHECK_EQ(image.segments.size(), 1);
    CHECK_EQ(image.segments[0].address, 0x8000);
    CHECK_EQ(image.segments[0].bytes.size(), 4);
    CHECK_EQ(image.segments[0].bytes[3], 0xE5);
    CHECK_EQ(image.entry, 0x8000);
}

TEST(image_rejects_bad_checksum) {
    bool threw = false;
    try {
        parse_srec("S1078000B000B1E533\n");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

namespace {

bool rejects(const std::string& text) {
    try {
        parse_srec(text);
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

}  // namespace

TEST(image_rejects_malformed_records) {
    // Each has a correct checksum, so it is the named defect that is caught.
    CHECK(rejects("S101FE\n"));          // an S1 with no room for its 2-byte address
    CHECK(rejects("S3030000FC\n"));      // an S3 with 2 address bytes where it needs 4
    CHECK(rejects("S2040000\n"));        // count says 4, carries 3
    CHECK(rejects("S4030000FC\n"));      // no such record type
    CHECK(rejects("S1078000B000B1E5\n")); // the checksum byte missing
    CHECK(rejects("S1078000B000B1E53\n"));  // odd length
    CHECK(rejects("S1078000B0G0B1E532\n"));  // not hex
    CHECK(rejects("X1078000B000B1E532\n"));  // not an S-record
}
