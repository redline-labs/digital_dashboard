// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading .trc traces, one fixture per format version.
//
// The version fixtures are PEAK's own example blocks, copied out of the format
// specification rather than written here, so what is being pinned is the format
// and not this implementation's idea of it. Every one of v1.0 through v1.3 and
// v2.1 failed the previous parser completely.
//
// The rest of the file is the malformed and awkward input that AGENTS.md asks
// for and that the old tests had none of. The load-bearing case is
// `bad_line_in_the_middle`: one unparseable row used to return zero frames for
// the whole file.

#include "can_trc/trc.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using namespace can::trc;

namespace
{

// Checks report through the exit code, never assert(): the project adds
// -DNDEBUG unconditionally, not just in Release, so an assert() here compiles
// to nothing and the test passes whatever the parser did.
int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

struct ReadResult
{
    std::vector<Record> records;
    ReadStats stats;
    Version version { Version::V1_0 };
    uint64_t startTimeUnixUs { 0 };
};

ReadResult read_all(const std::string& text)
{
    ReadResult result;
    auto reader = Reader::from_string(text);
    for (;;)
    {
        auto record = reader->next();
        if (!record.has_value())
        {
            SPDLOG_ERROR("reader failed: {}", record.error().message);
            ++failures;
            break;
        }
        if (!record->has_value())
        {
            break;
        }
        result.records.push_back(**record);
    }
    result.stats = reader->stats();
    result.version = reader->header().version;
    result.startTimeUnixUs = reader->header().startTimeUnixUs;
    return result;
}

bool data_is(const Record& record, const std::vector<uint8_t>& expected)
{
    if (record.frame.len != expected.size())
    {
        return false;
    }
    for (size_t i = 0; i < expected.size(); ++i)
    {
        if (record.frame.data[i] != expected[i])
        {
            return false;
        }
    }
    return true;
}

// --- the specification's own examples ----------------------------------------

// PEAK spec, "Version 1.0". No header keywords at all: the record kind has to
// be worked out from the identifier being FFFFFFFF and from the ERROR and RTR
// words standing where the payload would be.
constexpr const char kV1_0[] =
    ";##########################################################################\n"
    ";   C:\\TraceFile.trc\n"
    ";\n"
    ";    CAN activities recorded by PCAN Explorer\n"
    ";    Start time: 11.09.2002 16:00:20.682\n"
    ";    PCAN-Net: PCI1\n"
    ";\n"
    ";----+-   ---+--- ----+--- + -+ -- -- ...\n"
    "     1)      1841      0001 8 00 00 00 00 00 00 00 00\n"
    "     2)      1842      0008 4 ERROR 00 19 08 08\n"
    "     3)      1843 FFFFFFFF 4 00 00 00 04 -- -- -- -- BUSLIGHT\n"
    "     4)      1844      0100 3 RTR\n";

void test_v1_0()
{
    const ReadResult result = read_all(kV1_0);
    expect(result.version == Version::V1_0, "1.0: no $FILEVERSION means version 1.0");
    expect(result.records.size() == 4, "1.0: four records");
    expect(result.stats.badLines == 0, "1.0: no bad lines");
    if (result.records.size() != 4)
    {
        return;
    }

    const Record& data = result.records[0];
    expect(data.kind == RecordKind::Data, "1.0: first record is data");
    expect(data.number == 1, "1.0: the ')' after the message number is not part of it");
    expect(data.offsetUs == 1841000, "1.0: a v1.0 offset is whole milliseconds");
    expect(data.frame.id == 0x0001 && !data.frame.isExtended, "1.0: 11-bit identifier");
    expect(data_is(data, { 0, 0, 0, 0, 0, 0, 0, 0 }), "1.0: eight zero bytes");

    const Record& error = result.records[1];
    expect(error.kind == RecordKind::ErrorFrame, "1.0: 'ERROR' marks an error frame");
    expect(error.frame.isError, "1.0: and the frame says so");
    expect(data_is(error, { 0x00, 0x19, 0x08, 0x08 }), "1.0: error frame keeps its four bytes");

    const Record& warning = result.records[2];
    expect(warning.kind == RecordKind::HardwareStatus,
           "1.0: identifier FFFFFFFF marks an error warning");
    expect(data_is(warning, { 0x00, 0x00, 0x00, 0x04 }),
           "1.0: the payload stops at the '--' padding, before the BUSLIGHT flag name");

    const Record& remote = result.records[3];
    expect(remote.kind == RecordKind::Remote, "1.0: 'RTR' marks a remote request");
    expect(remote.frame.isRTR, "1.0: and the frame says so");
    expect(remote.frame.len == 3, "1.0: a remote request carries the length it asks for");
}

// PEAK spec, "Version 1.1". $FILEVERSION and $STARTTIME arrive, the type and
// the direction share one column, and the offset has a single fractional digit
// meaning tenths of a millisecond.
constexpr const char kV1_1[] =
    ";$FILEVERSION=1.1\n"
    ";$STARTTIME=37704.5364870833\n"
    ";\n"
    ";   Start time: 24.03.2003 12:52:32.484\n"
    ";---+--   ----+---- --+-- ----+--- + -+ -- -- -- -- -- -- --\n"
    "      1)     1059.9 Rx          0300 7 00 00 00 00 04 00 00\n"
    "      3)     1298.9 Tx          0400 2 00 00\n"
    "      5)     1346.8 Warng FFFFFFFF 4 00 00 00 04 BUSLIGHT\n"
    "      6)     1349.2 Error       0008 4 00 19 08 08\n"
    "      7)     1352.7 Rx          0100 3 RTR\n";

void test_v1_1()
{
    const ReadResult result = read_all(kV1_1);
    expect(result.version == Version::V1_1, "1.1: version comes from $FILEVERSION");
    expect(result.records.size() == 5, "1.1: five records");
    expect(result.stats.badLines == 0, "1.1: no bad lines");
    if (result.records.size() != 5)
    {
        return;
    }

    expect(result.records[0].offsetUs == 1059900,
           "1.1: one fractional digit is tenths of a millisecond, so 1059.9 is 1059900 us");
    expect(!result.records[0].isTx, "1.1: Rx");
    expect(result.records[1].isTx, "1.1: Tx");
    expect(result.records[2].kind == RecordKind::HardwareStatus, "1.1: Warng");
    expect(data_is(result.records[2], { 0x00, 0x00, 0x00, 0x04 }),
           "1.1: the BUSLIGHT flag name is not payload");
    expect(result.records[3].kind == RecordKind::ErrorFrame, "1.1: Error");
    expect(result.records[4].kind == RecordKind::Remote, "1.1: RTR after an Rx type");
    expect(result.startTimeUnixUs != 0, "1.1: $STARTTIME was read");
}

// PEAK spec, "Version 1.2". A Bus column appears, and the offset gains
// microsecond resolution.
constexpr const char kV1_2[] =
    ";$FILEVERSION=1.2\n"
    ";$STARTTIME=39878.6772258947;\n"
    ";   Start time: 06.03.2009 16:15:12.317.3\n"
    "      1)     1059.900 1 Rx           0300 7 00 00 00 00 04 00 00\n"
    "      5)     1346.834 1 Warng FFFFFFFF 4 00 00 00 04 BUSLIGHT\n"
    "      7)     1352.743 2 Rx           0100 3 RTR\n";

void test_v1_2()
{
    const ReadResult result = read_all(kV1_2);
    expect(result.version == Version::V1_2, "1.2: version");
    expect(result.records.size() == 3, "1.2: three records");
    expect(result.stats.badLines == 0, "1.2: no bad lines");
    if (result.records.size() != 3)
    {
        return;
    }
    expect(result.records[0].offsetUs == 1059900, "1.2: three fractional digits are microseconds");
    expect(result.records[0].bus == 1, "1.2: bus column");
    expect(result.records[2].bus == 2, "1.2: a second bus");
    expect(result.startTimeUnixUs != 0,
           "1.2: a $STARTTIME with PCAN-Explorer 5's stray trailing ';' still parses");
}

// PEAK spec, "Version 1.3". A J1939 Reserved column appears between the
// identifier and the length, holding '-' for plain CAN.
constexpr const char kV1_3[] =
    ";$FILEVERSION=1.3\n"
    ";$STARTTIME=40023.5245451516\n"
    ";   Start time: 29.07.2009 12:35:20.701.0\n"
    ";---+-- ------+------ +- --+-- ----+--- +- -+-- -+ -- -- -- -- -- -- --\n"
    "     1)      1059.900 1 Rx         0300 - 7      00 00 00 00 04 00 00\n"
    "     5)      1346.834 1 Warng FFFFFFFF - 4       00 00 00 04 BUSLIGHT\n"
    "     6)      1349.222 1 Error      0008 - 4      00 19 08 08\n"
    "     7)      1352.743 1 Rx         0100 - 3      RTR\n";

void test_v1_3()
{
    const ReadResult result = read_all(kV1_3);
    expect(result.version == Version::V1_3, "1.3: version");
    expect(result.records.size() == 4, "1.3: four records");
    expect(result.stats.badLines == 0, "1.3: no bad lines");
    if (result.records.size() != 4)
    {
        return;
    }
    expect(!result.records[0].destinationAddress.has_value(),
           "1.3: '-' in the Reserved column means no J1939 destination");
    expect(result.records[1].kind == RecordKind::HardwareStatus, "1.3: Warng");
    expect(result.records[3].kind == RecordKind::Remote, "1.3: RTR");
}

// PEAK spec, "Version 2.0". Type and direction split apart, and -- the part
// that breaks naive parsers -- ER, ST and EC lines have no identifier and no
// length column at all, so every later token shifts left.
constexpr const char kV2_0[] =
    ";$FILEVERSION=2.0\n"
    ";$STARTTIME=42209.4075997106\n"
    ";$COLUMNS=N,O,T,I,d,l,D\n"
    ";\n"
    ";   Start time: 24.07.2015 09:46:56.615.0\n"
    ";   Generated by PCAN-View v4.0.29.426\n"
    ";---+-- ------+------ +- --+----- +- +- +- -- -- -- -- -- -- --\n"
    "      1      1059.900 DT      0300 Rx 7 00 00 00 00 04 00 00\n"
    "      3      1298.945 DT      0400 Tx 2 00 00\n"
    "      5      1334.416 FD      0500 Tx 12 01 02 03 04 05 06 07 08 09 0A 0B 0C\n"
    "      6      1334.522 ER           Rx    04 00 02 00 00\n"
    "      7      1334.531 ST           Rx    00 00 00 08\n"
    "      8      1334.643 EC           Rx    02 02\n"
    "      9      1335.156 DT 18EFC034 Tx 8 01 02 03 04 05 06 07 08\n"
    "     10      1336.543 RR      0100 Rx 3\n";

void test_v2_0()
{
    const ReadResult result = read_all(kV2_0);
    expect(result.version == Version::V2_0, "2.0: version");
    expect(result.records.size() == 8, "2.0: eight records");
    expect(result.stats.badLines == 0, "2.0: no bad lines");
    expect(result.records.size() == 8 ? true : false, "2.0: record count");
    if (result.records.size() != 8)
    {
        return;
    }

    expect(result.records[0].kind == RecordKind::Data && !result.records[0].frame.isFD,
           "2.0: DT is a classic data frame");
    expect(result.records[1].isTx, "2.0: the direction column is separate now");

    const Record& fd = result.records[2];
    expect(fd.frame.isFD && !fd.frame.isBRS && !fd.frame.isESI, "2.0: FD sets only the FD flag");
    expect(fd.frame.len == 12, "2.0: an 'l' column is a byte count, so 12 means twelve bytes");

    expect(result.records[3].kind == RecordKind::ErrorFrame,
           "2.0: ER parses even though its identifier and length columns are absent");
    expect(data_is(result.records[3], { 0x04, 0x00, 0x02, 0x00, 0x00 }),
           "2.0: an error frame's five bytes survive");
    expect(result.records[4].kind == RecordKind::HardwareStatus, "2.0: ST");
    expect(data_is(result.records[4], { 0x00, 0x00, 0x00, 0x08 }), "2.0: status word");
    expect(result.records[5].kind == RecordKind::ErrorCounter, "2.0: EC");
    expect(data_is(result.records[5], { 0x02, 0x02 }), "2.0: both error counters");

    const Record& extended = result.records[6];
    expect(extended.frame.isExtended && extended.frame.id == 0x18EFC034,
           "2.0: an eight-digit identifier is 29-bit");

    expect(result.records[7].kind == RecordKind::Remote, "2.0: RR");
    expect(result.records[7].frame.len == 3,
           "2.0: RR carries the length being asked for and no payload");
}

// PEAK spec, "Version 2.1". A Bus column, a Reserved column, the EV text
// record -- and an 'L' column, which is a CAN FD length *code* rather than a
// byte count. The FD record here is the same twelve-byte frame v2.0 wrote as
// 'l'=12; v2.1 writes 'L'=9 for it.
constexpr const char kV2_1[] =
    ";$FILEVERSION=2.1\n"
    ";$STARTTIME=41766.4648963872\n"
    ";$COLUMNS=N,O,T,B,I,d,R,L,D\n"
    ";\n"
    ";   Generated by PCAN-Explorer v6.0.0\n"
    ";---+-- ------+------ +- +- --+----- +- +- +--- +- -- -- -- -- -- -- --\n"
    "       1      1059.900 DT 1       0300 Rx - 7     00 00 00 00 04 00 00\n"
    "       5      1334.416 FD 1       0500 Tx - 9     01 02 03 04 05 06 07 08 09 0A 0B 0C\n"
    "       6      1334.222 ER 1          - Rx - 5     04 00 02 00 00\n"
    "       7      1334.224 EV 1 User-defined event for bus 1\n"
    "       8      1334.225 EV - User-defined event for all busses\n"
    "       9      1334.231 ST 1          - Rx - 4     00 00 00 08\n"
    "      11      1334.643 EC 1          - Rx - 2     02 02\n"
    "      12      1335.156 DT 1 18EFC034 Tx - 8       01 02 03 04 05 06 07 08\n"
    "      13      1336.543 RR 1       0100 Rx - 3\n";

void test_v2_1()
{
    const ReadResult result = read_all(kV2_1);
    expect(result.version == Version::V2_1, "2.1: version");
    expect(result.records.size() == 9, "2.1: nine records");
    expect(result.stats.badLines == 0, "2.1: no bad lines");
    if (result.records.size() != 9)
    {
        return;
    }

    expect(result.records[0].bus == 1, "2.1: bus column");

    const Record& fd = result.records[1];
    expect(fd.frame.isFD, "2.1: FD");
    expect(fd.frame.len == 12,
           "2.1: an 'L' column is a length code -- 9 means twelve bytes, not nine");

    expect(result.records[2].kind == RecordKind::ErrorFrame,
           "2.1: ER keeps a '-' placeholder where v2.0 dropped the column");
    expect(data_is(result.records[2], { 0x04, 0x00, 0x02, 0x00, 0x00 }), "2.1: error bytes");

    expect(result.records[3].kind == RecordKind::Event, "2.1: EV");
    expect(result.records[3].bus == 1, "2.1: an event can name a bus");
    expect(result.records[3].event == "User-defined event for bus 1",
           "2.1: an event's text runs to the end of the line");
    expect(result.records[4].bus == 0,
           "2.1: '-' in the bus column is an event tied to no bus");
    expect(result.records[4].event == "User-defined event for all busses",
           "2.1: and its text still reads");

    expect(result.records[7].frame.isExtended, "2.1: 29-bit identifier");
    expect(result.records[8].kind == RecordKind::Remote && result.records[8].frame.len == 3,
           "2.1: RR");
}

// PEAK spec, "Version 3.0". Read for what it shares with 2.1; its CAN XL
// records are counted and skipped, because helpers::CanFrame has no VCID, SDT
// or AF and carries 64 bytes rather than 2048.
constexpr const char kV3_0[] =
    ";$FILEVERSION=3.0\n"
    ";$STARTTIME=45400.635924928486\n"
    ";$COLUMNS=N,O,T,B,I,d,R,V,S,A,r,s,L,D\n"
    "       1      1059.900 DT 1       300 Rx - - - -           - - 7    "
    "00 00 00 00 04 00 00\n"
    "      14      2131.122 XL 1       100 Tx - 00 03 00000456 0 0 3     03 11 22 33\n"
    "      16      2277.024 XL 1       006 Tx - 00 07 00000333 0 0 11    "
    "00 01 02 03 04 05 06 07 08 09 0A 0B\n";

void test_v3_0()
{
    const ReadResult result = read_all(kV3_0);
    expect(result.version == Version::V3_0, "3.0: version");
    expect(result.records.size() == 1, "3.0: the one non-XL record is returned");
    expect(result.stats.unsupported == 2, "3.0: both XL records are counted as unsupported");
    expect(result.stats.badLines == 0,
           "3.0: an XL record is unsupported, not malformed -- the difference matters, "
           "because badLines is what says a trace has holes");
    if (result.records.empty())
    {
        return;
    }
    expect(result.records[0].frame.id == 0x300 && !result.records[0].frame.isExtended,
           "3.0: 11-bit identifiers are three hex digits here, not four");
}

// --- the traps ---------------------------------------------------------------

// The width of the identifier token is the only thing in the file that says
// whether a frame was 11-bit or 29-bit. The old parser read the number and
// discarded the width, so these two lines produced identical frames.
void test_id_width_carries_ide()
{
    const ReadResult result = read_all(";$FILEVERSION=2.0\n"
                                       ";$COLUMNS=N,O,T,I,d,l,D\n"
                                       "1 0.000 DT     0123 Rx 1 AA\n"
                                       "2 0.001 DT 00000123 Rx 1 BB\n");
    expect(result.records.size() == 2, "id width: two records");
    if (result.records.size() != 2)
    {
        return;
    }
    expect(result.records[0].frame.id == 0x123 && !result.records[0].frame.isExtended,
           "id width: four digits is an 11-bit identifier");
    expect(result.records[1].frame.id == 0x123 && result.records[1].frame.isExtended,
           "id width: eight digits is a 29-bit identifier with the same value, and they are "
           "different messages on a real bus");
}

// Every FD length code, against can::dlc_to_length. Reading an 'L' column as a
// byte count gives a frame that is the wrong length with nothing to say so.
void test_fd_length_codes()
{
    std::string text = ";$FILEVERSION=2.1\n;$COLUMNS=N,O,T,B,I,d,R,L,D\n";
    const uint8_t lengths[] = { 12, 16, 20, 24, 32, 48, 64 };
    for (uint8_t dlc = 9; dlc <= 15; ++dlc)
    {
        std::string line = std::to_string(dlc) + " 0.000 FD 1 0100 Rx - " + std::to_string(dlc)
            + "    ";
        for (uint8_t i = 0; i < lengths[dlc - 9]; ++i)
        {
            line += (i == 0 ? "" : " ");
            line += "00";
        }
        text += line + "\n";
    }

    const ReadResult result = read_all(text);
    expect(result.records.size() == 7, "fd lengths: seven records");
    if (result.records.size() != 7)
    {
        return;
    }
    for (size_t i = 0; i < 7; ++i)
    {
        expect(result.records[i].frame.len == lengths[i],
               "fd lengths: length code " + std::to_string(i + 9) + " means "
                   + std::to_string(lengths[i]) + " bytes");
        expect(result.records[i].frame.isFD, "fd lengths: and the FD flag is set");
    }
}

void test_fd_flags()
{
    const ReadResult result = read_all(";$FILEVERSION=2.1\n"
                                       ";$COLUMNS=N,O,T,B,I,d,R,L,D\n"
                                       "1 0.000 FD 1 0100 Rx - 1 AA\n"
                                       "2 0.001 FB 1 0100 Rx - 1 AA\n"
                                       "3 0.002 FE 1 0100 Rx - 1 AA\n"
                                       "4 0.003 BI 1 0100 Rx - 1 AA\n");
    expect(result.records.size() == 4, "fd flags: four records");
    if (result.records.size() != 4)
    {
        return;
    }
    expect(result.records[0].frame.isFD && !result.records[0].frame.isBRS
               && !result.records[0].frame.isESI,
           "fd flags: FD");
    expect(result.records[1].frame.isBRS && !result.records[1].frame.isESI, "fd flags: FB is BRS");
    expect(!result.records[2].frame.isBRS && result.records[2].frame.isESI, "fd flags: FE is ESI");
    expect(result.records[3].frame.isBRS && result.records[3].frame.isESI,
           "fd flags: BI is both");
}

// The one that matters most. A single unparseable row used to make the whole
// file return nothing, which for a 24,000-line trace is a total outage caused
// by one bad character.
void test_bad_line_in_the_middle()
{
    const ReadResult result = read_all(";$FILEVERSION=2.0\n"
                                       ";$COLUMNS=N,O,T,I,d,l,D\n"
                                       "1 0.000 DT 0100 Rx 1 AA\n"
                                       "2 0.001 XX not a record at all\n"
                                       "3 0.002 DT 0101 Rx 1 BB\n"
                                       "4 0.003 DT 0102 Rx 1 CC\n");
    expect(result.records.size() == 3,
           "recovery: a bad line costs that line and nothing else");
    expect(result.stats.badLines == 1, "recovery: and is counted");
    if (result.records.size() != 3)
    {
        return;
    }
    expect(result.records[0].frame.id == 0x0100 && result.records[1].frame.id == 0x0101
               && result.records[2].frame.id == 0x0102,
           "recovery: the records either side of it are intact and in order");
}

// mock_data/data/pdm32_log.trc runs from 4294967270.343 ms to 4295008779.456 ms
// -- it crosses 2^32 milliseconds inside one file. A 32-bit accumulator wraps
// here, and a float has seven significant digits for a value needing thirteen.
void test_offset_beyond_32_bits()
{
    const ReadResult result = read_all(";$FILEVERSION=2.0\n"
                                       ";$COLUMNS=N,O,T,I,d,l,D\n"
                                       "1 4294967269.343 DT 0500 Rx 1 AA\n"
                                       "2 4295008779.456 DT 0500 Rx 1 BB\n");
    expect(result.records.size() == 2, "wide offsets: two records");
    if (result.records.size() != 2)
    {
        return;
    }
    expect(result.records[0].offsetUs == 4294967269343ull,
           "wide offsets: an offset just under 2^32 ms keeps every digit");
    expect(result.records[1].offsetUs == 4295008779456ull,
           "wide offsets: and one past 2^32 ms does not wrap");
    expect(result.records[1].offsetUs > result.records[0].offsetUs,
           "wide offsets: time still moves forwards across the boundary");
}

void test_start_time_conversion()
{
    expect(ole_date_to_unix_us(25569.0) == 0,
           "start time: the OLE epoch offset puts 25569 days at the UNIX epoch");
    expect(ole_date_to_unix_us(25570.0) == 86400000000ull, "start time: one day later");
    expect(ole_date_to_unix_us(1.0) == 0,
           "start time: a date before the UNIX epoch clamps rather than wrapping");

    const uint64_t now = 1760000000000000ull;
    expect(ole_date_to_unix_us(unix_us_to_ole_date(now)) / 1000u == now / 1000u,
           "start time: the conversion round-trips to the millisecond");

    const ReadResult result = read_all(";$FILEVERSION=2.0\n"
                                       ";$STARTTIME=25570.0\n"
                                       ";$COLUMNS=N,O,T,I,d,l,D\n"
                                       "1 1.500 DT 0100 Rx 1 AA\n");
    expect(result.startTimeUnixUs == 86400000000ull, "start time: read from the header");
    if (result.records.size() == 1)
    {
        expect(result.records[0].frame.timestampUs == 86400000000ull + 1500ull,
               "start time: a record's frame gets an absolute time, start plus offset");
    }
}

void test_whitespace_and_terminators()
{
    const ReadResult crlf = read_all(";$FILEVERSION=2.0\r\n"
                                     ";$COLUMNS=N,O,T,I,d,l,D\r\n"
                                     "1 0.000 DT 0100 Rx 1 AA\r\n"
                                     "2 0.001 DT 0101 Rx 1 BB\r\n");
    expect(crlf.records.size() == 2 && crlf.stats.badLines == 0,
           "terminators: CRLF, which is what the format actually specifies");

    const ReadResult noFinalNewline = read_all(";$FILEVERSION=2.0\n"
                                               ";$COLUMNS=N,O,T,I,d,l,D\n"
                                               "1 0.000 DT 0100 Rx 1 AA");
    expect(noFinalNewline.records.size() == 1 && noFinalNewline.stats.badLines == 0,
           "terminators: a last line with no newline after it still counts");

    const ReadResult blanks = read_all(";$FILEVERSION=2.0\n"
                                       ";$COLUMNS=N,O,T,I,d,l,D\n"
                                       "\n"
                                       "1 0.000 DT 0100 Rx 1 AA\n"
                                       "   \n"
                                       "2 0.001 DT 0101 Rx 1 BB\n"
                                       "\n");
    expect(blanks.records.size() == 2 && blanks.stats.badLines == 0,
           "terminators: blank lines are not records and are not errors");

    const ReadResult headerOnly
        = read_all(";$FILEVERSION=2.0\n;$COLUMNS=N,O,T,I,d,l,D\n;   nothing else\n");
    expect(headerOnly.records.empty() && headerOnly.stats.badLines == 0,
           "terminators: a header with no records is a valid empty trace");

    const ReadResult empty = read_all("");
    expect(empty.records.empty() && empty.stats.badLines == 0 && empty.stats.lines == 0,
           "terminators: an empty file is an empty trace");
}

void test_malformed_lines()
{
    const ReadResult truncated = read_all(";$FILEVERSION=2.0\n"
                                          ";$COLUMNS=N,O,T,I,d,l,D\n"
                                          "1 0.000 DT 0100\n");
    expect(truncated.records.empty() && truncated.stats.badLines == 1,
           "malformed: a line that stops before the layout does");

    const ReadResult shortPayload = read_all(";$FILEVERSION=2.0\n"
                                             ";$COLUMNS=N,O,T,I,d,l,D\n"
                                             "1 0.000 DT 0100 Rx 8 AA BB\n");
    expect(shortPayload.records.empty() && shortPayload.stats.badLines == 1,
           "malformed: a length of 8 with two bytes present is rejected, not silently "
           "shortened to a frame that looks real");

    const ReadResult longPayload = read_all(";$FILEVERSION=2.0\n"
                                            ";$COLUMNS=N,O,T,I,d,l,D\n"
                                            "1 0.000 DT 0100 Rx 1 AA BB CC\n");
    expect(longPayload.records.empty() && longPayload.stats.badLines == 1,
           "malformed: extra payload bytes are rejected rather than dropped");

    const ReadResult badId = read_all(";$FILEVERSION=2.0\n"
                                      ";$COLUMNS=N,O,T,I,d,l,D\n"
                                      "1 0.000 DT 0G00 Rx 1 AA\n");
    expect(badId.records.empty() && badId.stats.badLines == 1,
           "malformed: a non-hexadecimal identifier");

    const ReadResult wideId = read_all(";$FILEVERSION=2.0\n"
                                       ";$COLUMNS=N,O,T,I,d,l,D\n"
                                       "1 0.000 DT 000100 Rx 1 AA\n");
    expect(wideId.records.empty() && wideId.stats.badLines == 1,
           "malformed: six hex digits is neither the 11-bit form nor the 29-bit one");

    const ReadResult bigId = read_all(";$FILEVERSION=2.0\n"
                                      ";$COLUMNS=N,O,T,I,d,l,D\n"
                                      "1 0.000 DT 3FFFFFFF Rx 1 AA\n");
    expect(bigId.records.empty() && bigId.stats.badLines == 1,
           "malformed: an identifier too large for the 29 bits its width declares");

    const ReadResult badDirection = read_all(";$FILEVERSION=2.0\n"
                                             ";$COLUMNS=N,O,T,I,d,l,D\n"
                                             "1 0.000 DT 0100 Up 1 AA\n");
    expect(badDirection.records.empty() && badDirection.stats.badLines == 1,
           "malformed: a direction that is neither Rx nor Tx");

    const ReadResult badBus = read_all(";$FILEVERSION=2.1\n"
                                       ";$COLUMNS=N,O,T,B,I,d,R,L,D\n"
                                       "1 0.000 DT 17 0100 Rx - 1 AA\n");
    expect(badBus.records.empty() && badBus.stats.badLines == 1,
           "malformed: the format allows buses 1 to 16");

    const ReadResult badTime = read_all(";$FILEVERSION=2.0\n"
                                        ";$COLUMNS=N,O,T,I,d,l,D\n"
                                        "1 later DT 0100 Rx 1 AA\n");
    expect(badTime.records.empty() && badTime.stats.badLines == 1,
           "malformed: an offset that is not a number");

    // A record longer than helpers::CanFrame can hold. Rejecting it is the
    // point: the old parser clamped to 64 and produced a frame that had lost
    // bytes with nothing to indicate it.
    std::string large = ";$FILEVERSION=2.1\n;$COLUMNS=N,O,T,B,I,d,R,l,D\n1 0.000 DT 1 0100 Rx - 96";
    for (int i = 0; i < 96; ++i)
    {
        large += " 00";
    }
    const ReadResult tooLong = read_all(large + "\n");
    expect(tooLong.records.empty() && tooLong.stats.badLines == 1,
           "malformed: a J1939 large message does not fit a 64-byte frame and is refused "
           "rather than truncated");
}

void test_columns_declaration()
{
    // A file whose declared layout is not the version's default. This is what a
    // fixed grammar cannot do, and what made v2.1 unreadable before.
    const ReadResult noNumber = read_all(";$FILEVERSION=2.1\n"
                                         ";$COLUMNS=O,T,B,I,d,l,D\n"
                                         "0.000 DT 1 0100 Rx 1 AA\n");
    expect(noNumber.records.size() == 1 && noNumber.stats.badLines == 0,
           "columns: the message number column is optional");
    if (noNumber.records.size() == 1)
    {
        expect(noNumber.records[0].number == 0 && noNumber.records[0].frame.id == 0x100,
               "columns: and everything after it still lands in the right field");
    }

    auto bothLengths = parse_columns("N,O,T,I,d,l,L,D");
    expect(!bothLengths.has_value(),
           "columns: a file cannot declare both a byte count and a length code");

    auto unknown = parse_columns("N,O,T,Z,D");
    expect(!unknown.has_value(), "columns: an unknown column identifier is refused");

    auto good = parse_columns("N,O,T,B,I,d,R,L,D");
    expect(good.has_value() && good->order.size() == 9, "columns: the v2.1 default parses");

    // An unreadable $COLUMNS must not take the file with it: the reader warns
    // and falls back to the version's layout.
    const ReadResult fallback = read_all(";$FILEVERSION=2.0\n"
                                         ";$COLUMNS=N,O,T,Z,d,l,D\n"
                                         "1 0.000 DT 0100 Rx 1 AA\n");
    expect(fallback.records.size() == 1,
           "columns: a bad $COLUMNS falls back to the version default rather than "
           "failing the file");
}

// The two enum-to-string switches. Trivial in themselves, but they are what
// -Wswitch-enum is protecting: a value added to either enum has to show up as a
// build failure rather than as "?" in a diagnostic nobody reads carefully.
void test_enum_strings()
{
    const Version versions[] = { Version::V1_0, Version::V1_1, Version::V1_2, Version::V1_3,
                                 Version::V2_0, Version::V2_1, Version::V3_0 };
    for (const Version version : versions)
    {
        const std::string text = version_text(version);
        expect(text != "?" && !text.empty(),
               "enum strings: every version has a $FILEVERSION spelling");
        auto parsed = parse_version(text);
        expect(parsed.has_value() && *parsed == version,
               "enum strings: and it parses back to the same version -- " + text);
    }

    const RecordKind kinds[] = { RecordKind::Data,         RecordKind::Remote,
                                 RecordKind::ErrorFrame,   RecordKind::HardwareStatus,
                                 RecordKind::ErrorCounter, RecordKind::Event,
                                 RecordKind::Unsupported };
    for (const RecordKind kind : kinds)
    {
        expect(std::string(to_string(kind)) != "?",
               "enum strings: every record kind has a name");
    }

    // The default layouts, which are what a v1.x file relies on entirely --
    // those versions have no $COLUMNS line to declare anything.
    for (const Version version : versions)
    {
        const Columns columns = default_columns(version);
        expect(!columns.order.empty(), "enum strings: every version has a default layout");
        expect(columns.has(ColumnId::Offset) && columns.has(ColumnId::Data),
               "enum strings: and every layout has at least a time and a payload");
        expect(!(columns.has(ColumnId::Length) && columns.has(ColumnId::Dlc)),
               "enum strings: and never both a byte count and a length code");
    }
}

// The offset field, at its edges. Everything here used to be a silent wrong
// number rather than a rejected line.
void test_offset_edges()
{
    auto offset_of = [](const std::string& text) -> std::optional<uint64_t> {
        const ReadResult result = read_all(";$FILEVERSION=2.0\n;$COLUMNS=N,O,T,I,d,l,D\n"
                                           "1 "
                                           + text + " DT 0100 Rx 1 AA\n");
        if (result.records.size() != 1)
        {
            return std::nullopt;
        }
        return result.records[0].offsetUs;
    };

    expect(offset_of("1") == 1000ull, "offset: a bare integer is milliseconds");
    expect(offset_of("1.1") == 1100ull, "offset: one fractional digit is tenths of a millisecond");
    expect(offset_of("1.01") == 1010ull, "offset: two is hundredths");
    expect(offset_of("1.001") == 1001ull, "offset: three is microseconds");
    expect(offset_of("1.123456") == 1123ull,
           "offset: digits finer than a microsecond are dropped, not rejected -- the value "
           "is valid, it is just finer than this field holds");
    expect(offset_of("0.000") == 0ull, "offset: zero");

    expect(!offset_of("1.").has_value(), "offset: a decimal point with no digits after it");
    expect(!offset_of("1.5a").has_value(), "offset: a non-digit in the fraction");
    expect(!offset_of("-1.000").has_value(), "offset: a negative time offset");
    expect(!offset_of("").has_value(), "offset: an empty field");

    // Fits a uint64 but overflows when scaled to microseconds. Rejecting beats
    // wrapping to a small number that looks like the start of the trace.
    expect(!offset_of("18446744073709552.000").has_value(),
           "offset: a value that would overflow when scaled to microseconds is refused "
           "rather than wrapped");
}

void test_header_keyword_errors()
{
    // A $STARTTIME that is not a number must not take the file with it: the
    // records are still readable, they just have no absolute time.
    const ReadResult badStart = read_all(";$FILEVERSION=2.0\n"
                                         ";$STARTTIME=not-a-date\n"
                                         ";$COLUMNS=N,O,T,I,d,l,D\n"
                                         "1 0.000 DT 0100 Rx 1 AA\n");
    expect(badStart.records.size() == 1 && badStart.stats.badLines == 0,
           "header: an unreadable $STARTTIME does not cost the records");
    expect(badStart.startTimeUnixUs == 0, "header: it just leaves the absolute time unknown");
    if (badStart.records.size() == 1)
    {
        expect(badStart.records[0].frame.timestampUs == 0,
               "header: and the frames carry no timestamp rather than a wrong one");
    }

    // A keyword with no '=' at all, and one this build does not know. Both are
    // ignored rather than fatal -- the format gains keywords over time and an
    // older reader has to keep working.
    const ReadResult odd = read_all(";$FILEVERSION=2.0\n"
                                    ";$COLUMNS=N,O,T,I,d,l,D\n"
                                    ";$SOMETHINGNEW=17\n"
                                    ";$MALFORMED\n"
                                    "1 0.000 DT 0100 Rx 1 AA\n");
    expect(odd.records.size() == 1 && odd.stats.badLines == 0,
           "header: an unknown or malformed $-keyword is ignored, not fatal");

    // A $COLUMNS that arrives after records have already been read. Legal
    // nowhere, but it must not corrupt what was already parsed.
    const ReadResult late = read_all(";$FILEVERSION=2.0\n"
                                     ";$COLUMNS=N,O,T,I,d,l,D\n"
                                     "1 0.000 DT 0100 Rx 1 AA\n"
                                     ";$COLUMNS=O,T,I,d,l,D\n"
                                     "0.001 DT 0101 Rx 1 BB\n");
    expect(late.records.size() == 2 && late.stats.badLines == 0,
           "header: a $COLUMNS partway through takes effect from there");
}

void test_columns_errors()
{
    auto multiChar = parse_columns("NO,O,T,D");
    expect(!multiChar.has_value(),
           "columns: an identifier is a single letter, so 'NO' is refused rather than "
           "silently read as 'N'");

    auto duplicate = parse_columns("N,N,O,T,D");
    expect(!duplicate.has_value(), "columns: the same column twice is refused");

    auto empty = parse_columns("");
    expect(!empty.has_value(), "columns: an empty $COLUMNS is refused");

    auto noOffset = parse_columns("N,I,d,D");
    expect(!noOffset.has_value(),
           "columns: a layout with no time and no type column is not a trace");
}

void test_version_parsing()
{
    expect(parse_version("2.1").has_value() && *parse_version("2.1") == Version::V2_1,
           "version: 2.1");
    expect(!parse_version("4.0").has_value(),
           "version: an unknown version is refused rather than guessed -- guessing picks a "
           "column layout and yields plausible wrong frames");

    // A file claiming a version this build does not know still reads with the
    // layout it had, rather than becoming unreadable.
    const ReadResult unknownVersion = read_all(";$FILEVERSION=9.9\n"
                                               ";$COLUMNS=N,O,T,I,d,l,D\n"
                                               "1 0.000 DT 0100 Rx 1 AA\n");
    expect(unknownVersion.records.size() == 1,
           "version: an unknown $FILEVERSION warns and keeps reading");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);

    test_v1_0();
    test_v1_1();
    test_v1_2();
    test_v1_3();
    test_v2_0();
    test_v2_1();
    test_v3_0();

    test_id_width_carries_ide();
    test_fd_length_codes();
    test_fd_flags();
    test_bad_line_in_the_middle();
    test_offset_beyond_32_bits();
    test_start_time_conversion();
    test_whitespace_and_terminators();
    test_malformed_lines();
    test_columns_declaration();
    test_columns_errors();
    test_header_keyword_errors();
    test_offset_edges();
    test_enum_strings();
    test_version_parsing();

    if (failures == 0)
    {
        SPDLOG_INFO("can_trc read tests passed");
        return EXIT_SUCCESS;
    }
    SPDLOG_ERROR("{} check(s) failed", failures);
    return EXIT_FAILURE;
}
