// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bridge's .trc recording tap.
//
// Reading the file back rather than inspecting its text: what matters is that a
// frame put in comes out the same frame, and the reader is the thing that will
// actually be used to check a recording later.

#include "trc_recorder.h"

#include "can_trc/trc.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace can_bridge;

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

std::vector<can::trc::Record> read_back(const std::filesystem::path& path, uint64_t& badLines)
{
    std::vector<can::trc::Record> records;
    auto reader = can::trc::Reader::open(path.string());
    if (!reader.has_value())
    {
        SPDLOG_ERROR("cannot reopen {}: {}", path.string(), reader.error().message);
        ++failures;
        return records;
    }
    for (;;)
    {
        auto record = (*reader)->next();
        if (!record.has_value() || !record->has_value())
        {
            break;
        }
        records.push_back(**record);
    }
    badLines = (*reader)->stats().badLines;
    return records;
}

helpers::CanFrame frame_with(uint32_t id, bool extended, const std::vector<uint8_t>& payload)
{
    helpers::CanFrame frame;
    frame.id = id;
    frame.isExtended = extended;
    frame.len = static_cast<uint8_t>(payload.size());
    for (size_t i = 0; i < payload.size(); ++i)
    {
        frame.data[i] = payload[i];
    }
    return frame;
}

can::trc::BusInfo bus_info()
{
    can::trc::BusInfo info;
    info.bus = 1;
    info.name = "can0";
    info.connection = "virtual:bench";
    info.bitrateBps = 500000;
    return info;
}

void test_both_directions_land_in_the_file()
{
    const auto path = temp_path("can_bridge_recorder_both.trc");

    {
        auto recorder = TrcRecorder::create(path.string(), 1, bus_info());
        expect(recorder.has_value(), "directions: the recorder opens");
        if (!recorder.has_value())
        {
            return;
        }

        (*recorder)->record_rx(frame_with(0x100, false, { 0x01, 0x02 }));
        (*recorder)->record_tx(frame_with(0x200, false, { 0x03 }));
        (*recorder)->record_rx(frame_with(0x18EFC034, true, { 1, 2, 3, 4, 5, 6, 7, 8 }));

        helpers::CanFrame fd = frame_with(0x300, false, std::vector<uint8_t>(24, 0xAB));
        fd.isFD = true;
        fd.isBRS = true;
        (*recorder)->record_rx(fd);

        helpers::CanFrame remote = frame_with(0x400, false, {});
        remote.isRTR = true;
        remote.len = 4;
        (*recorder)->record_tx(remote);

        helpers::CanFrame errorFrame = frame_with(0, false, { 0x04, 0x00, 0x02, 0x00, 0x00 });
        errorFrame.isError = true;
        (*recorder)->record_rx(errorFrame);

        // The destructor drains the queue and flushes, which is what makes a
        // trace usable after a SIGINT rather than truncated at whatever the
        // last full buffer was.
    }

    uint64_t badLines = 0;
    const std::vector<can::trc::Record> records = read_back(path, badLines);

    expect(badLines == 0, "directions: what the recorder wrote reads back cleanly");
    expect(records.size() == 6,
           "directions: six records, got " + std::to_string(records.size()));
    if (records.size() != 6)
    {
        return;
    }

    expect(!records[0].isTx && records[0].frame.id == 0x100, "directions: a received frame is Rx");
    expect(records[1].isTx && records[1].frame.id == 0x200,
           "directions: a transmitted frame is Tx, which is the whole reason recording is a "
           "tap on the channel and not a send()-only backend");
    expect(records[2].frame.isExtended && records[2].frame.id == 0x18EFC034,
           "directions: the extended flag survives");
    expect(records[3].frame.isFD && records[3].frame.isBRS && records[3].frame.len == 24,
           "directions: an FD frame keeps its flags and its length");
    expect(records[4].kind == can::trc::RecordKind::Remote && records[4].frame.len == 4,
           "directions: a remote request is RR, and keeps the length it asked for");
    expect(records[5].kind == can::trc::RecordKind::ErrorFrame,
           "directions: a controller error is an error record, not traffic from a device "
           "that does not exist");

    for (const can::trc::Record& record : records)
    {
        expect(record.bus == 1, "directions: every record carries the configured bus");
    }

    std::filesystem::remove(path);
}

void test_order_and_offsets()
{
    const auto path = temp_path("can_bridge_recorder_order.trc");

    {
        auto recorder = TrcRecorder::create(path.string(), 1, bus_info());
        if (!recorder.has_value())
        {
            expect(false, "offsets: the recorder opens");
            return;
        }
        for (uint32_t i = 0; i < 50; ++i)
        {
            (*recorder)->record_rx(frame_with(0x100 + i, false, { static_cast<uint8_t>(i) }));
        }
    }

    uint64_t badLines = 0;
    const std::vector<can::trc::Record> records = read_back(path, badLines);

    expect(records.size() == 50, "offsets: fifty records");
    if (records.size() != 50)
    {
        return;
    }

    bool ordered = true;
    bool monotonic = true;
    for (size_t i = 0; i < records.size(); ++i)
    {
        ordered = ordered && records[i].frame.id == 0x100 + i;
        ordered = ordered && records[i].number == i + 1;
        if (i > 0)
        {
            monotonic = monotonic && records[i].offsetUs >= records[i - 1].offsetUs;
        }
    }
    expect(ordered, "offsets: records keep the order they were handed over in, and are numbered");
    expect(monotonic, "offsets: and their offsets never go backwards");

    std::filesystem::remove(path);
}

// Frame timestamps in this tree do not share an epoch -- SocketCAN gives the
// kernel's wall clock, a PCAN adapter gives its own uptime. Differences are
// still real, so the recorder pins an origin at the first stamped frame and
// measures from there.
void test_hardware_timestamps_become_offsets()
{
    const auto path = temp_path("can_bridge_recorder_stamps.trc");

    // A device-uptime clock: nowhere near a wall clock, but internally
    // consistent, which is all the recorder needs.
    const uint64_t origin = 987654321000ull;

    {
        auto recorder = TrcRecorder::create(path.string(), 1, bus_info());
        if (!recorder.has_value())
        {
            expect(false, "stamps: the recorder opens");
            return;
        }
        for (uint32_t i = 0; i < 5; ++i)
        {
            helpers::CanFrame frame = frame_with(0x100, false, { 0x00 });
            frame.timestampUs = origin + i * 10000ull; // 10 ms apart
            (*recorder)->record_rx(frame);
        }
    }

    uint64_t badLines = 0;
    const std::vector<can::trc::Record> records = read_back(path, badLines);

    expect(records.size() == 5, "stamps: five records");
    if (records.size() != 5)
    {
        return;
    }

    // The first frame anchors the timeline; every later one is exactly its
    // hardware distance from that anchor.
    const uint64_t base = records[0].offsetUs;
    bool spacingHeld = true;
    for (size_t i = 0; i < records.size(); ++i)
    {
        spacingHeld = spacingHeld && records[i].offsetUs == base + i * 10000ull;
    }
    expect(spacingHeld,
           "stamps: a device clock with no relation to wall time still yields the right "
           "gaps, because the recorder measures differences rather than trusting the epoch");

    std::filesystem::remove(path);
}

// A stamp that goes backwards means a wrapped counter or two backends' clocks
// reaching one recorder. Emitting it would make the trace non-monotonic, and
// several things in this tree binary-search buffers that assume it is not.
void test_backwards_stamp_falls_back_to_arrival()
{
    const auto path = temp_path("can_bridge_recorder_backwards.trc");

    {
        auto recorder = TrcRecorder::create(path.string(), 1, bus_info());
        if (!recorder.has_value())
        {
            expect(false, "backwards: the recorder opens");
            return;
        }
        helpers::CanFrame first = frame_with(0x100, false, { 0x00 });
        first.timestampUs = 5000000ull;
        (*recorder)->record_rx(first);

        helpers::CanFrame wrapped = frame_with(0x101, false, { 0x00 });
        wrapped.timestampUs = 1000ull; // the counter wrapped
        (*recorder)->record_rx(wrapped);

        helpers::CanFrame after = frame_with(0x102, false, { 0x00 });
        after.timestampUs = 5010000ull;
        (*recorder)->record_rx(after);
    }

    uint64_t badLines = 0;
    const std::vector<can::trc::Record> records = read_back(path, badLines);

    expect(records.size() == 3, "backwards: three records");
    if (records.size() != 3)
    {
        return;
    }
    expect(records[1].offsetUs >= records[0].offsetUs
               && records[2].offsetUs >= records[1].offsetUs,
           "backwards: a stamp older than the origin does not produce an offset that runs "
           "backwards");

    std::filesystem::remove(path);
}

// The recorder must never block the pump thread or a zenoh callback, so a queue
// it cannot drain has to drop. The count is what tells you the trace has a hole.
void test_overflow_drops_and_counts()
{
    const auto path = temp_path("can_bridge_recorder_overflow.trc");

    uint64_t dropped = 0;
    uint64_t recorded = 0;
    {
        auto recorder = TrcRecorder::create(path.string(), 1, bus_info());
        if (!recorder.has_value())
        {
            expect(false, "overflow: the recorder opens");
            return;
        }

        // Far more than the queue holds, pushed as fast as one thread can, so
        // the writer cannot keep up.
        const size_t count = 200000;
        for (size_t i = 0; i < count; ++i)
        {
            (*recorder)->record_rx(frame_with(0x100, false, { 0x00 }));
        }
        dropped = (*recorder)->dropped();
        recorded = (*recorder)->recorded();
    }

    uint64_t badLines = 0;
    const std::vector<can::trc::Record> records = read_back(path, badLines);

    expect(badLines == 0, "overflow: whatever did get written is still a valid trace");
    expect(records.size() + dropped == 200000,
           "overflow: every frame is either in the file or counted as dropped -- "
               + std::to_string(records.size()) + " written, " + std::to_string(dropped)
               + " dropped");
    (void)recorded;

    std::filesystem::remove(path);
}

void test_two_threads_do_not_interleave_badly()
{
    const auto path = temp_path("can_bridge_recorder_threads.trc");

    {
        auto recorder = TrcRecorder::create(path.string(), 1, bus_info());
        if (!recorder.has_value())
        {
            expect(false, "threads: the recorder opens");
            return;
        }

        // The real shape: the pump thread records receives while a zenoh
        // callback thread records transmits.
        std::thread pump([&] {
            for (uint32_t i = 0; i < 500; ++i)
            {
                (*recorder)->record_rx(frame_with(0x100, false, { 0x00 }));
            }
        });
        std::thread callback([&] {
            for (uint32_t i = 0; i < 500; ++i)
            {
                (*recorder)->record_tx(frame_with(0x200, false, { 0x00 }));
            }
        });
        pump.join();
        callback.join();
    }

    uint64_t badLines = 0;
    const std::vector<can::trc::Record> records = read_back(path, badLines);

    expect(badLines == 0, "threads: no torn or interleaved lines");
    expect(records.size() == 1000, "threads: every record from both threads is present, got "
               + std::to_string(records.size()));

    size_t rx = 0;
    size_t tx = 0;
    bool monotonic = true;
    for (size_t i = 0; i < records.size(); ++i)
    {
        records[i].isTx ? ++tx : ++rx;
        if (i > 0)
        {
            monotonic = monotonic && records[i].offsetUs >= records[i - 1].offsetUs;
        }
    }
    expect(rx == 500 && tx == 500, "threads: both directions are complete");
    expect(monotonic,
           "threads: offsets are taken on the producing thread, so two producers still "
           "yield one non-decreasing timeline");

    std::filesystem::remove(path);
}

void test_unwritable_path_fails_at_create()
{
    auto recorder
        = TrcRecorder::create("/nonexistent-directory/trace.trc", 1, bus_info());
    expect(!recorder.has_value(),
           "unwritable: a path that cannot be created fails at create(), so the bridge can "
           "log it and carry on rather than discovering it per frame");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);

    test_both_directions_land_in_the_file();
    test_order_and_offsets();
    test_hardware_timestamps_become_offsets();
    test_backwards_stamp_falls_back_to_arrival();
    test_overflow_drops_and_counts();
    test_two_threads_do_not_interleave_badly();
    test_unwritable_path_fails_at_create();

    if (failures == 0)
    {
        SPDLOG_INFO("can_bridge trc recorder tests passed");
        return EXIT_SUCCESS;
    }
    SPDLOG_ERROR("{} check(s) failed", failures);
    return EXIT_FAILURE;
}
