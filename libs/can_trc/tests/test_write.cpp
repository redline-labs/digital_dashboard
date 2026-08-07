// SPDX-License-Identifier: GPL-3.0-or-later
//
// Writing .trc traces, and reading back what was written.
//
// The round trip is the strong assertion here: every field the old parser threw
// away -- the timestamp, the direction, the bus, the extended flag, the FD
// length -- has to survive being written out and read in again, which it cannot
// do if either side is guessing.
//
// The two real traces in mock_data/ are read as part of this, asserting both
// their exact record counts and that nothing in them was rejected. Nothing in
// the tree read those files before, which is why a parser that returned zero
// frames for a whole file went unnoticed.

#include "can_trc/trc.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace can::trc;

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

std::filesystem::path temp_path(const char* name)
{
    return std::filesystem::temp_directory_path() / name;
}

std::string slurp(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::vector<Record> read_all_from(const std::filesystem::path& path, ReadStats& stats,
                                  FileHeader& header)
{
    std::vector<Record> records;
    auto reader = Reader::open(path.string());
    if (!reader.has_value())
    {
        SPDLOG_ERROR("cannot open {}: {}", path.string(), reader.error().message);
        ++failures;
        return records;
    }
    for (;;)
    {
        auto record = (*reader)->next();
        if (!record.has_value())
        {
            SPDLOG_ERROR("read failed: {}", record.error().message);
            ++failures;
            break;
        }
        if (!record->has_value())
        {
            break;
        }
        records.push_back(**record);
    }
    stats = (*reader)->stats();
    header = (*reader)->header();
    return records;
}

// Everything a record carries, compared field by field. A round trip that
// preserved four of six fields would otherwise pass.
bool same_record(const Record& a, const Record& b, std::string& why)
{
    auto differs = [&](const char* field) {
        why = field;
        return false;
    };
    if (a.kind != b.kind)                       { return differs("kind"); }
    if (a.number != b.number)                   { return differs("number"); }
    if (a.offsetUs != b.offsetUs)               { return differs("offset"); }
    if (a.bus != b.bus)                         { return differs("bus"); }
    if (a.isTx != b.isTx)                       { return differs("direction"); }
    if (a.destinationAddress != b.destinationAddress) { return differs("destination"); }
    if (a.event != b.event)                     { return differs("event text"); }
    if (a.frame.id != b.frame.id)               { return differs("identifier"); }
    if (a.frame.isExtended != b.frame.isExtended) { return differs("extended"); }
    if (a.frame.isFD != b.frame.isFD)           { return differs("fd"); }
    if (a.frame.isBRS != b.frame.isBRS)         { return differs("brs"); }
    if (a.frame.isESI != b.frame.isESI)         { return differs("esi"); }
    if (a.frame.isRTR != b.frame.isRTR)         { return differs("rtr"); }
    if (a.frame.len != b.frame.len)             { return differs("length"); }
    for (size_t i = 0; i < a.frame.len; ++i)
    {
        if (a.frame.data[i] != b.frame.data[i])
        {
            return differs("payload");
        }
    }
    return true;
}

Record data_record(uint64_t number, uint64_t offsetUs, uint32_t id, bool extended,
                   const std::vector<uint8_t>& payload)
{
    Record record;
    record.kind = RecordKind::Data;
    record.number = number;
    record.offsetUs = offsetUs;
    record.bus = 1;
    record.frame.id = id;
    record.frame.isExtended = extended;
    record.frame.len = static_cast<uint8_t>(payload.size());
    for (size_t i = 0; i < payload.size(); ++i)
    {
        record.frame.data[i] = payload[i];
    }
    return record;
}

void test_header_shape()
{
    const auto path = temp_path("can_trc_header.trc");
    WriterOptions options;
    options.startTimeUnixUs = 86400000000ull; // one day past the UNIX epoch
    options.generatedBy = "can_trc test";
    options.buses.push_back(BusInfo { 1, "bench", "virtual:bench", 500000, 0 });

    auto writer = Writer::create(path.string(), options);
    expect(writer.has_value(), "header: the writer opens");
    if (!writer.has_value())
    {
        return;
    }
    (void)(*writer)->write(data_record(1, 1500, 0x100, false, { 0xAA }));
    (void)(*writer)->flush();
    writer->reset();

    const std::string text = slurp(path);

    // The three $-keywords are the only lines a reader is obliged to
    // understand, so they are checked literally.
    expect(text.find(";$FILEVERSION=2.1\r\n") == 0, "header: $FILEVERSION comes first");
    expect(text.find(";$COLUMNS=N,O,T,B,I,d,R,l,D\r\n") != std::string::npos,
           "header: the declared layout matches what write() actually emits");
    expect(text.find(";$STARTTIME=25570.") != std::string::npos,
           "header: $STARTTIME is an OLE Automation date, so one day past the UNIX epoch "
           "is 25570");
    expect(text.find("\r\n") != std::string::npos,
           "header: lines are terminated CR/LF, as the format specifies");
    expect(text.find("Generated by can_trc test") != std::string::npos,
           "header: the generator is named");
    expect(text.find("bench") != std::string::npos && text.find("500 kbit/s") != std::string::npos,
           "header: the bus table lists the bus and its bit rate");

    std::filesystem::remove(path);
}

void test_round_trip_synthetic()
{
    std::vector<Record> written;

    written.push_back(data_record(1, 0, 0x100, false, { 0x01, 0x02 }));
    written.push_back(data_record(2, 1500, 0x18EFC034, true, { 1, 2, 3, 4, 5, 6, 7, 8 }));

    // Every FD shape, because the type letter is where the flags live.
    for (int i = 0; i < 4; ++i)
    {
        Record fd = data_record(static_cast<uint64_t>(3 + i), 2000 + 100 * static_cast<uint64_t>(i),
                                0x200, false, std::vector<uint8_t>(48, 0x5A));
        fd.frame.isFD = true;
        fd.frame.isBRS = (i & 1) != 0;
        fd.frame.isESI = (i & 2) != 0;
        written.push_back(fd);
    }

    Record remote;
    remote.kind = RecordKind::Remote;
    remote.number = 7;
    remote.offsetUs = 3000;
    remote.bus = 1;
    remote.frame.id = 0x123;
    remote.frame.isRTR = true;
    remote.frame.len = 5;
    written.push_back(remote);

    Record errorFrame;
    errorFrame.kind = RecordKind::ErrorFrame;
    errorFrame.number = 8;
    errorFrame.offsetUs = 3500;
    errorFrame.bus = 1;
    errorFrame.frame.isError = true;
    errorFrame.frame.len = 5;
    for (uint8_t i = 0; i < 5; ++i)
    {
        errorFrame.frame.data[i] = static_cast<uint8_t>(0x10 + i);
    }
    written.push_back(errorFrame);

    Record status;
    status.kind = RecordKind::HardwareStatus;
    status.number = 9;
    status.offsetUs = 4000;
    status.bus = 1;
    status.frame.isError = true;
    status.frame.len = 4;
    status.frame.data[3] = 0x08;
    written.push_back(status);

    Record counter;
    counter.kind = RecordKind::ErrorCounter;
    counter.number = 10;
    counter.offsetUs = 4500;
    counter.bus = 1;
    counter.frame.isError = true;
    counter.frame.len = 2;
    counter.frame.data[0] = 0x02;
    counter.frame.data[1] = 0x03;
    written.push_back(counter);

    Record event;
    event.kind = RecordKind::Event;
    event.number = 11;
    event.offsetUs = 5000;
    event.bus = 1;
    event.event = "something worth recording";
    written.push_back(event);

    Record busless = data_record(12, 5500, 0x7FF, false, {});
    busless.bus = 0;
    written.push_back(busless);

    Record j1939 = data_record(13, 6000, 0x18EF0000, true, { 0xFF });
    j1939.destinationAddress = 34;
    written.push_back(j1939);

    const auto path = temp_path("can_trc_round_trip.trc");
    WriterOptions options;
    options.startTimeUnixUs = 1700000000000000ull;
    auto writer = Writer::create(path.string(), options);
    expect(writer.has_value(), "round trip: the writer opens");
    if (!writer.has_value())
    {
        return;
    }
    for (const Record& record : written)
    {
        auto result = (*writer)->write(record);
        expect(result.has_value(), "round trip: every record writes");
    }
    expect((*writer)->recordsWritten() == written.size(),
           "round trip: the writer's own count agrees with what was handed to it");
    (void)(*writer)->flush();
    writer->reset();

    ReadStats stats;
    FileHeader header;
    const std::vector<Record> readBack = read_all_from(path, stats, header);

    expect(header.version == Version::V2_1, "round trip: what was written reads as 2.1");
    expect(stats.badLines == 0, "round trip: nothing written is unreadable");
    expect(readBack.size() == written.size(),
           "round trip: " + std::to_string(written.size()) + " records out, "
               + std::to_string(readBack.size()) + " back");

    const size_t count = std::min(written.size(), readBack.size());
    for (size_t i = 0; i < count; ++i)
    {
        std::string why;
        expect(same_record(written[i], readBack[i], why),
               "round trip: record " + std::to_string(i) + " differs in " + why);
    }

    // $STARTTIME goes through an OLE Automation date, which is a double. It
    // holds sub-microsecond precision over these ranges, but the comparison is
    // to the millisecond because nothing here needs better and a float equality
    // on a converted date is a test that fails for the wrong reason.
    const uint64_t drift = header.startTimeUnixUs > options.startTimeUnixUs
        ? header.startTimeUnixUs - options.startTimeUnixUs
        : options.startTimeUnixUs - header.startTimeUnixUs;
    expect(drift < 1000, "round trip: $STARTTIME survives the OLE date conversion");

    std::filesystem::remove(path);
}

void test_id_width_is_written()
{
    const auto path = temp_path("can_trc_id_width.trc");
    auto writer = Writer::create(path.string(), WriterOptions {});
    if (!writer.has_value())
    {
        expect(false, "id width: the writer opens");
        return;
    }
    (void)(*writer)->write(data_record(1, 0, 0x123, false, { 0xAA }));
    (void)(*writer)->write(data_record(2, 1000, 0x123, true, { 0xBB }));
    (void)(*writer)->flush();
    writer->reset();

    const std::string text = slurp(path);
    expect(text.find(" 0123 ") != std::string::npos,
           "id width: an 11-bit identifier is written with four hex digits");
    expect(text.find(" 00000123 ") != std::string::npos,
           "id width: a 29-bit identifier with eight, which is the only thing in the file "
           "that distinguishes them");

    ReadStats stats;
    FileHeader header;
    const std::vector<Record> records = read_all_from(path, stats, header);
    expect(records.size() == 2, "id width: both read back");
    if (records.size() == 2)
    {
        expect(!records[0].frame.isExtended && records[1].frame.isExtended,
               "id width: and the flag survives the trip");
    }

    std::filesystem::remove(path);
}

void test_remote_writes_no_payload()
{
    const auto path = temp_path("can_trc_remote.trc");
    auto writer = Writer::create(path.string(), WriterOptions {});
    if (!writer.has_value())
    {
        expect(false, "remote: the writer opens");
        return;
    }
    Record remote;
    remote.kind = RecordKind::Remote;
    remote.number = 1;
    remote.bus = 1;
    remote.frame.id = 0x100;
    remote.frame.isRTR = true;
    remote.frame.len = 8;
    (void)(*writer)->write(remote);
    (void)(*writer)->flush();
    writer->reset();

    const std::string text = slurp(path);
    const size_t rr = text.find(" RR ");
    expect(rr != std::string::npos, "remote: written as RR");
    if (rr != std::string::npos)
    {
        const size_t begin = text.rfind('\n', rr) + 1;
        const std::string line = text.substr(begin, text.find('\r', rr) - begin);

        // Counting the fields rather than searching for "00": the identifier
        // 0100 contains that, so a substring test here passes for the wrong
        // reason and keeps passing when the payload comes back.
        std::vector<std::string> fields;
        std::istringstream stream(line);
        std::string field;
        while (stream >> field)
        {
            fields.push_back(field);
        }
        expect(fields.size() == 8,
               "remote: the line has the eight declared columns and nothing after the "
               "length, got " + std::to_string(fields.size()) + ": '" + line + "'");
        if (fields.size() >= 8)
        {
            expect(fields[2] == "RR", "remote: the type column");
            expect(fields[7] == "8",
                   "remote: it still declares the length it is asking for, and that is the "
                   "last field -- an RR carries no payload of its own");
        }
    }

    std::filesystem::remove(path);
}

void test_writer_refuses_other_versions()
{
    WriterOptions options;
    options.version = Version::V1_1;
    auto writer = Writer::create(temp_path("can_trc_v11.trc").string(), options);
    expect(!writer.has_value(),
           "writer: asking for a version this build does not write fails at create() rather "
           "than producing a file labelled 1.1 with 2.1 columns in it");
}

// The two real traces shipped in mock_data/. Nothing read them before.
void test_real_traces()
{
    struct Fixture
    {
        const char* name;
        size_t records;
    };
    const Fixture fixtures[] = {
        { "pdm32_log.trc", 24156 },
        { "racegrade_tc8.trc", 6600 },
    };

    for (const Fixture& fixture : fixtures)
    {
        const std::filesystem::path path
            = std::filesystem::path(CAN_TRC_MOCK_DATA_DIR) / fixture.name;

        ReadStats stats;
        FileHeader header;
        const std::vector<Record> records = read_all_from(path, stats, header);

        expect(header.version == Version::V2_0,
               std::string(fixture.name) + ": PCAN-View 5 writes 2.0");
        expect(records.size() == fixture.records,
               std::string(fixture.name) + ": " + std::to_string(fixture.records)
                   + " records, got " + std::to_string(records.size()));
        // The assertion that would have caught the old parser: a whole real
        // file, and not one line of it refused.
        expect(stats.badLines == 0,
               std::string(fixture.name) + ": every line parsed");
        expect(header.startTimeUnixUs != 0,
               std::string(fixture.name) + ": $STARTTIME was read");

        if (records.empty())
        {
            continue;
        }

        // Offsets must not go backwards. pdm32_log crosses 2^32 milliseconds
        // partway through, which is precisely where a narrower accumulator
        // would wrap and where this assertion would fail.
        bool monotonic = true;
        uint64_t previous = records.front().offsetUs;
        for (const Record& record : records)
        {
            monotonic = monotonic && record.offsetUs >= previous;
            previous = record.offsetUs;
        }
        expect(monotonic, std::string(fixture.name) + ": offsets never go backwards");

        // Write it back out and read it again. This is where a dropped flag or
        // a mangled length shows up.
        const auto copy = temp_path("can_trc_real_copy.trc");
        WriterOptions options;
        options.startTimeUnixUs = header.startTimeUnixUs;
        options.generatedBy = "can_trc round trip";
        auto writer = Writer::create(copy.string(), options);
        if (!writer.has_value())
        {
            expect(false, std::string(fixture.name) + ": the writer opens");
            continue;
        }
        for (const Record& record : records)
        {
            (void)(*writer)->write(record);
        }
        (void)(*writer)->flush();
        writer->reset();

        ReadStats copyStats;
        FileHeader copyHeader;
        const std::vector<Record> reread = read_all_from(copy, copyStats, copyHeader);

        expect(copyStats.badLines == 0,
               std::string(fixture.name) + ": the copy is clean");
        expect(reread.size() == records.size(),
               std::string(fixture.name) + ": the copy has the same number of records");

        size_t mismatches = 0;
        std::string firstWhy;
        const size_t count = std::min(records.size(), reread.size());
        for (size_t i = 0; i < count; ++i)
        {
            std::string why;
            if (!same_record(records[i], reread[i], why))
            {
                if (mismatches == 0)
                {
                    firstWhy = why + " at record " + std::to_string(i);
                }
                ++mismatches;
            }
        }
        expect(mismatches == 0,
               std::string(fixture.name) + ": " + std::to_string(mismatches)
                   + " records changed across a 2.0 -> 2.1 -> 2.1 round trip; first: "
                   + firstWhy);

        // pdm32_log has both four- and eight-digit identifiers in it, so this
        // is real coverage of the flag rather than a synthetic case.
        size_t extended = 0;
        for (const Record& record : records)
        {
            extended += record.frame.isExtended ? 1u : 0u;
        }
        if (std::string(fixture.name) == "pdm32_log.trc")
        {
            expect(extended > 0,
                   "pdm32_log.trc: its 29-bit frames are recognised as 29-bit");
            expect(extended < records.size(),
                   "pdm32_log.trc: and its 11-bit frames are not");
        }

        std::filesystem::remove(copy);
    }
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);

    test_header_shape();
    test_round_trip_synthetic();
    test_id_width_is_written();
    test_remote_writes_no_payload();
    test_writer_refuses_other_versions();
    test_real_traces();

    if (failures == 0)
    {
        SPDLOG_INFO("can_trc write tests passed");
        return EXIT_SUCCESS;
    }
    SPDLOG_ERROR("{} check(s) failed", failures);
    return EXIT_FAILURE;
}
