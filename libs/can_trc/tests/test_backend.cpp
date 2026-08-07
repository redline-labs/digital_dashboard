// SPDX-License-Identifier: GPL-3.0-or-later
//
// The trc: replay backend.
//
// What is being pinned here is that a file behaves like a bus: frames come out
// in order, spaced the way they were recorded, a multi-bus trace splits into one
// channel per bus, and a stop() does not have to wait out the gap to the next
// frame before it takes effect. That last one is the difference between a node
// that shuts down and a node that appears to hang.

#include "can_trc/trc_backend.h"

#include "can/backend.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace can;
using namespace std::chrono;

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

std::filesystem::path write_temp(const char* name, const std::string& contents)
{
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
    out.close();
    return path;
}

// Three buses, so the bus column is doing something. One day past the UNIX
// epoch, so the absolute timestamps are a number this test can name.
const std::string kMultiBus =
    ";$FILEVERSION=2.1\n"
    ";$STARTTIME=25570.0\n"
    ";$COLUMNS=N,O,T,B,I,d,R,l,D\n"
    "1     0.000 DT 1 0100 Rx - 1 01\n"
    "2     1.000 DT 2 0200 Rx - 1 02\n"
    "3     2.000 DT 1 0101 Rx - 1 03\n"
    "4     3.000 DT 3 0300 Tx - 1 04\n"
    "5     4.000 DT 1 0102 Rx - 1 05\n";

std::vector<helpers::CanFrame> drain(Channel& channel, size_t want, Duration timeout)
{
    std::vector<helpers::CanFrame> frames;
    std::array<helpers::CanFrame, 8> batch;
    const auto deadline = steady_clock::now() + milliseconds(5000);
    while (frames.size() < want && steady_clock::now() < deadline)
    {
        auto count = channel.receive(batch, timeout);
        if (!count.has_value())
        {
            SPDLOG_ERROR("receive failed: {}", count.error().message);
            ++failures;
            break;
        }
        if (*count == 0)
        {
            break;
        }
        for (size_t i = 0; i < *count; ++i)
        {
            frames.push_back(batch[i]);
        }
    }
    // A receive fills the whole batch when frames are waiting, so stop at the
    // number asked for -- otherwise a looping trace hands back a batch's worth
    // and the count means nothing.
    if (frames.size() > want)
    {
        frames.resize(want);
    }
    return frames;
}

Registry registry_with(const trc::ReplayOptions& options)
{
    Registry registry;
    registry.add(trc::make_trc_backend(options));
    return registry;
}

void test_reads_every_bus()
{
    const auto path = write_temp("can_trc_backend_all.trc", kMultiBus);

    trc::ReplayOptions options;
    options.paced = false;
    const Registry registry = registry_with(options);

    auto channel = registry.open("trc:" + path.string(), OpenOptions {});
    expect(channel.has_value(), "all buses: the channel opens");
    if (!channel.has_value())
    {
        SPDLOG_ERROR("{}", channel.error().message);
        return;
    }

    const std::vector<helpers::CanFrame> frames = drain(**channel, 5, Duration { 100 });
    expect(frames.size() == 5,
           "all buses: an unqualified trc:<path> reads the whole trace, got "
               + std::to_string(frames.size()));
    if (frames.size() != 5)
    {
        return;
    }
    expect(frames[0].id == 0x100 && frames[1].id == 0x200 && frames[2].id == 0x101
               && frames[3].id == 0x300 && frames[4].id == 0x102,
           "all buses: in recorded order");

    // The one backend in this tree that can produce a real UNIX-epoch stamp:
    // the trace carries an absolute $STARTTIME and each record's offset from it.
    expect(frames[0].timestampUs == 86400000000ull,
           "all buses: the first frame's timestamp is $STARTTIME plus its offset");
    expect(frames[4].timestampUs == 86400000000ull + 4000ull,
           "all buses: and the last one four milliseconds later");

    std::filesystem::remove(path);
}

// A trace's Bus column maps onto ChannelId::channel, which is what makes a
// multi-bus file several can::Channels rather than a special case.
void test_bus_filter()
{
    const auto path = write_temp("can_trc_backend_bus.trc", kMultiBus);

    trc::ReplayOptions options;
    options.paced = false;
    const Registry registry = registry_with(options);

    auto busOne = registry.open("trc:" + path.string() + "/1", OpenOptions {});
    expect(busOne.has_value(), "bus filter: trc:<path>/1 opens");
    if (busOne.has_value())
    {
        const std::vector<helpers::CanFrame> frames = drain(**busOne, 3, Duration { 100 });
        expect(frames.size() == 3, "bus filter: bus 1 has three frames");
        if (frames.size() == 3)
        {
            expect(frames[0].id == 0x100 && frames[1].id == 0x101 && frames[2].id == 0x102,
                   "bus filter: and they are bus 1's");
        }
    }

    auto busThree = registry.open("trc:" + path.string() + "/3", OpenOptions {});
    expect(busThree.has_value(), "bus filter: trc:<path>/3 opens");
    if (busThree.has_value())
    {
        const std::vector<helpers::CanFrame> frames = drain(**busThree, 1, Duration { 100 });
        expect(frames.size() == 1, "bus filter: bus 3 has one frame");
        if (frames.size() == 1)
        {
            expect(frames[0].id == 0x300, "bus filter: the right one");
        }
    }

    auto busSeven = registry.open("trc:" + path.string() + "/7", OpenOptions {});
    expect(busSeven.has_value(), "bus filter: a bus with no records still opens");
    if (busSeven.has_value())
    {
        const std::vector<helpers::CanFrame> frames = drain(**busSeven, 1, Duration { 50 });
        expect(frames.empty(), "bus filter: and is simply quiet");
    }

    std::filesystem::remove(path);
}

void test_pacing()
{
    // 300 ms of trace, replayed at 10x, should take about 30 ms.
    const auto path = write_temp("can_trc_backend_pace.trc",
                                 ";$FILEVERSION=2.0\n"
                                 ";$COLUMNS=N,O,T,I,d,l,D\n"
                                 "1   0.000 DT 0100 Rx 1 01\n"
                                 "2 100.000 DT 0100 Rx 1 02\n"
                                 "3 200.000 DT 0100 Rx 1 03\n"
                                 "4 300.000 DT 0100 Rx 1 04\n");

    trc::ReplayOptions options;
    options.paced = true;
    options.speed = 10.0;
    const Registry registry = registry_with(options);

    auto channel = registry.open("trc:" + path.string(), OpenOptions {});
    expect(channel.has_value(), "pacing: the channel opens");
    if (!channel.has_value())
    {
        return;
    }

    const auto began = steady_clock::now();
    const std::vector<helpers::CanFrame> frames = drain(**channel, 4, Duration { 500 });
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - began);

    expect(frames.size() == 4, "pacing: all four frames arrive");
    // Generous bounds on both sides: the point is that pacing happens at all
    // and that it scales, not that a test machine's scheduler is precise.
    expect(elapsed >= milliseconds(20),
           "pacing: 300 ms of trace at 10x takes at least 20 ms, so the gaps are real, took "
               + std::to_string(elapsed.count()) + " ms");
    expect(elapsed < milliseconds(400),
           "pacing: and well under the recorded 300 ms, so the speed multiplier applies, took "
               + std::to_string(elapsed.count()) + " ms");

    std::filesystem::remove(path);
}

void test_unpaced_is_immediate()
{
    const auto path = write_temp("can_trc_backend_fast.trc",
                                 ";$FILEVERSION=2.0\n"
                                 ";$COLUMNS=N,O,T,I,d,l,D\n"
                                 "1     0.000 DT 0100 Rx 1 01\n"
                                 "2 60000.000 DT 0100 Rx 1 02\n");

    trc::ReplayOptions options;
    options.paced = false;
    const Registry registry = registry_with(options);

    auto channel = registry.open("trc:" + path.string(), OpenOptions {});
    if (!channel.has_value())
    {
        expect(false, "unpaced: the channel opens");
        return;
    }

    const auto began = steady_clock::now();
    const std::vector<helpers::CanFrame> frames = drain(**channel, 2, Duration { 100 });
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - began);

    expect(frames.size() == 2, "unpaced: both frames arrive");
    expect(elapsed < milliseconds(500),
           "unpaced: a minute of recorded gap costs nothing when pacing is off");

    std::filesystem::remove(path);
}

// A trace can have seconds between frames. If stop() only set a flag, shutdown
// would block until the next one was due -- the bug the virtual backend's
// condition variable exists to avoid, and the reason this channel has one too.
void test_stop_interrupts_a_paced_wait()
{
    const auto path = write_temp("can_trc_backend_stop.trc",
                                 ";$FILEVERSION=2.0\n"
                                 ";$COLUMNS=N,O,T,I,d,l,D\n"
                                 "1     0.000 DT 0100 Rx 1 01\n"
                                 "2 30000.000 DT 0100 Rx 1 02\n");

    trc::ReplayOptions options;
    options.paced = true;
    options.speed = 1.0;
    const Registry registry = registry_with(options);

    auto channel = registry.open("trc:" + path.string(), OpenOptions {});
    if (!channel.has_value())
    {
        expect(false, "stop: the channel opens");
        return;
    }

    // Take the first frame so the reader is sitting on the thirty-second gap.
    const std::vector<helpers::CanFrame> first = drain(**channel, 1, Duration { 200 });
    expect(first.size() == 1, "stop: the first frame arrives immediately");

    std::atomic<bool> returned { false };
    milliseconds waited { 0 };
    std::thread waiter([&] {
        std::array<helpers::CanFrame, 4> batch;
        const auto began = steady_clock::now();
        (void)(*channel)->receive(batch, Duration { 10000 });
        waited = duration_cast<milliseconds>(steady_clock::now() - began);
        returned = true;
    });

    std::this_thread::sleep_for(milliseconds(50));
    (void)(*channel)->stop();

    waiter.join();
    expect(returned.load(), "stop: the waiting receive returned");
    expect(waited < milliseconds(2000),
           "stop: it returned in " + std::to_string(waited.count())
               + " ms rather than waiting out the thirty-second gap to the next frame");

    std::filesystem::remove(path);
}

void test_loop_keeps_time_moving_forwards()
{
    const auto path = write_temp("can_trc_backend_loop.trc",
                                 ";$FILEVERSION=2.0\n"
                                 ";$STARTTIME=25570.0\n"
                                 ";$COLUMNS=N,O,T,I,d,l,D\n"
                                 "1 0.000 DT 0100 Rx 1 01\n"
                                 "2 1.000 DT 0101 Rx 1 02\n");

    trc::ReplayOptions options;
    options.paced = false;
    options.loop = true;
    const Registry registry = registry_with(options);

    auto channel = registry.open("trc:" + path.string(), OpenOptions {});
    if (!channel.has_value())
    {
        expect(false, "loop: the channel opens");
        return;
    }

    const std::vector<helpers::CanFrame> frames = drain(**channel, 6, Duration { 100 });
    expect(frames.size() == 6, "loop: the trace repeats");
    if (frames.size() != 6)
    {
        return;
    }
    expect(frames[0].id == 0x100 && frames[2].id == 0x100 && frames[4].id == 0x100,
           "loop: each pass starts at the beginning");

    // Several things in this tree binary-search a buffer that assumes
    // non-decreasing time and cannot detect otherwise, so a looped replay must
    // not hand back a clock that jumps backwards at the seam.
    bool monotonic = true;
    for (size_t i = 1; i < frames.size(); ++i)
    {
        monotonic = monotonic && frames[i].timestampUs > frames[i - 1].timestampUs;
    }
    expect(monotonic, "loop: timestamps keep increasing across the seam between passes");

    std::filesystem::remove(path);
}

void test_send_is_counted_not_pretended()
{
    const auto path = write_temp("can_trc_backend_send.trc", kMultiBus);
    const Registry registry = registry_with(trc::ReplayOptions {});

    auto channel = registry.open("trc:" + path.string(), OpenOptions {});
    if (!channel.has_value())
    {
        expect(false, "send: the channel opens");
        return;
    }

    helpers::CanFrame frame;
    frame.id = 0x123;
    frame.len = 1;
    (void)(*channel)->send(frame);
    (void)(*channel)->send(frame);

    expect((*channel)->statistics().txDropped == 2,
           "send: a file has nothing listening to it, and the frames are counted as dropped "
           "rather than reported as sent");

    std::filesystem::remove(path);
}

void test_missing_file_fails_at_open()
{
    const Registry registry = registry_with(trc::ReplayOptions {});

    auto channel = registry.open("trc:/nonexistent/nowhere.trc", OpenOptions {});
    expect(!channel.has_value(),
           "missing file: a path that is not there fails at open, where every other backend "
           "reports a missing device, rather than becoming a bus that is always quiet");
    if (!channel.has_value())
    {
        expect(channel.error().kind == Error::Kind::NotFound, "missing file: reported as NotFound");
    }

    auto empty = registry.open("trc:", OpenOptions {});
    expect(!empty.has_value(), "missing file: trc: with no path is refused");
}

// The rest of the can::Channel interface. Not decoration: can_bridge's
// fill_status() calls every one of these once a second, so a channel that
// answered wrongly would publish a wrong status for as long as it ran.
void test_channel_interface()
{
    const auto path = write_temp("can_trc_backend_iface.trc", kMultiBus);

    trc::ReplayOptions options;
    options.paced = false;
    const Registry registry = registry_with(options);

    OpenOptions open;
    open.bitrate.nominalBps = 500000;
    open.bitrate.dataBps = 2000000;
    open.listenOnly = true;

    auto channel = registry.open("trc:" + path.string() + "/2", open);
    expect(channel.has_value(), "interface: the channel opens");
    if (!channel.has_value())
    {
        return;
    }
    Channel& c = **channel;

    expect(c.id().backend == "trc" && c.id().device == path.string() && c.id().channel == 2,
           "interface: id() reports what was opened");
    expect(c.description().find(path.string()) != std::string::npos,
           "interface: description() names the file, which is what a log line needs");
    expect(c.supports_fd(), "interface: a trace can carry FD frames");

    // A file has no bit rate, but the one asked for is remembered so a recorder
    // downstream can put it in the header it writes.
    expect(c.bitrate().nominalBps == 500000 && c.bitrate().dataBps == 2000000,
           "interface: the bit rate asked for at open is remembered");
    expect(c.set_bitrate(can::Bitrate { 250000, 0, 0, 0 }).has_value(),
           "interface: set_bitrate succeeds rather than pretending a file can be reconfigured");
    expect(c.bitrate().nominalBps == 250000, "interface: and takes effect");

    expect(c.listen_only(), "interface: listen_only follows OpenOptions");
    expect(c.set_listen_only(false).has_value(), "interface: and can be changed");
    expect(!c.listen_only(), "interface: to the other value");

    expect(c.running(), "interface: OpenOptions.start defaulted to true, so it is running");
    expect(c.stop().has_value(), "interface: stop succeeds");
    expect(!c.running(), "interface: and running() says so");

    // A stopped channel delivers nothing, and starting it again resumes.
    std::array<helpers::CanFrame, 4> batch;
    auto whileStopped = c.receive(batch, Duration { 20 });
    expect(whileStopped.has_value() && *whileStopped == 0,
           "interface: a stopped channel is quiet rather than an error");
    expect(c.start().has_value(), "interface: start again");
    expect(c.running(), "interface: and it is running again");

    std::filesystem::remove(path);
}

// Opening without starting, which is what a caller does when it wants to
// configure before any frame arrives.
void test_open_without_starting()
{
    const auto path = write_temp("can_trc_backend_nostart.trc", kMultiBus);

    trc::ReplayOptions options;
    options.paced = false;
    const Registry registry = registry_with(options);

    OpenOptions open;
    open.start = false;

    auto channel = registry.open("trc:" + path.string(), open);
    expect(channel.has_value(), "no-start: the channel opens");
    if (!channel.has_value())
    {
        return;
    }
    expect(!(*channel)->running(), "no-start: but is not running");

    std::array<helpers::CanFrame, 4> batch;
    auto before = (*channel)->receive(batch, Duration { 20 });
    expect(before.has_value() && *before == 0, "no-start: and delivers nothing until started");

    expect((*channel)->start().has_value(), "no-start: start it");
    const std::vector<helpers::CanFrame> frames = drain(**channel, 5, Duration { 100 });
    expect(frames.size() == 5, "no-start: then the whole trace arrives");

    std::filesystem::remove(path);
}

// Neither virtual: nor trc: can be enumerated -- both exist only once something
// names one. can_bridge's --list depends on this being an empty list rather
// than an error.
void test_enumerate_is_empty()
{
    auto backend = trc::make_trc_backend(trc::ReplayOptions {});
    expect(backend->name() == "trc", "enumerate: the backend is named 'trc'");
    expect(backend->enumerate().empty(),
           "enumerate: a trace is not discoverable, so the list is empty rather than wrong");

    const Registry registry = registry_with(trc::ReplayOptions {});
    expect(registry.enumerate().empty(), "enumerate: and the registry agrees");
    expect(registry.backend_names().size() == 1 && registry.backend_names()[0] == "trc",
           "enumerate: while still reporting that the backend exists");
}

// A trace with no Bus column reports bus 0 on every record. An unqualified
// trc:<path> therefore reads it and trc:<path>/1 does not, which is the right
// way round: the file never claimed to be bus 1.
void test_no_bus_column()
{
    const auto path = write_temp("can_trc_backend_nobus.trc",
                                 ";$FILEVERSION=2.0\n"
                                 ";$COLUMNS=N,O,T,I,d,l,D\n"
                                 "1 0.000 DT 0100 Rx 1 01\n"
                                 "2 0.001 DT 0101 Rx 1 02\n");

    trc::ReplayOptions options;
    options.paced = false;
    const Registry registry = registry_with(options);

    auto unqualified = registry.open("trc:" + path.string(), OpenOptions {});
    expect(unqualified.has_value(), "no bus column: an unqualified path opens");
    if (unqualified.has_value())
    {
        expect(drain(**unqualified, 2, Duration { 100 }).size() == 2,
               "no bus column: and reads the whole trace");
    }

    auto qualified = registry.open("trc:" + path.string() + "/1", OpenOptions {});
    expect(qualified.has_value(), "no bus column: asking for bus 1 still opens");
    if (qualified.has_value())
    {
        expect(drain(**qualified, 1, Duration { 50 }).empty(),
               "no bus column: but delivers nothing, because the file never said it was "
               "bus 1 -- guessing would put a trace on a bus it was not recorded from");
    }

    std::filesystem::remove(path);
}

// Records that are not traffic. An error frame is a frame with isError set; an
// event is text with nowhere to put it and is skipped.
void test_events_and_error_records()
{
    const auto path = write_temp("can_trc_backend_events.trc",
                                 ";$FILEVERSION=2.1\n"
                                 ";$COLUMNS=N,O,T,B,I,d,R,l,D\n"
                                 "1 0.000 DT 1 0100 Rx - 1 01\n"
                                 "2 0.001 EV 1 a user-defined event\n"
                                 "3 0.002 ER 1    - Rx - 5 04 00 02 00 00\n"
                                 "4 0.003 ST 1    - Rx - 4 00 00 00 08\n"
                                 "5 0.004 EC 1    - Rx - 2 02 02\n"
                                 "6 0.005 DT 1 0101 Rx - 1 02\n");

    trc::ReplayOptions options;
    options.paced = false;
    const Registry registry = registry_with(options);

    auto channel = registry.open("trc:" + path.string(), OpenOptions {});
    expect(channel.has_value(), "events: the channel opens");
    if (!channel.has_value())
    {
        return;
    }

    const std::vector<helpers::CanFrame> frames = drain(**channel, 5, Duration { 100 });
    expect(frames.size() == 5,
           "events: five frames -- the event is skipped because a frame has nowhere to put "
           "its text, got " + std::to_string(frames.size()));
    if (frames.size() != 5)
    {
        return;
    }
    expect(!frames[0].isError && frames[0].id == 0x100, "events: the data frame");
    expect(frames[1].isError && frames[2].isError && frames[3].isError,
           "events: error, status and counter records all arrive flagged as errors rather "
           "than as traffic from a device that does not exist");
    expect(frames[4].id == 0x101, "events: and the data frame after them");

    const Statistics stats = (*channel)->statistics();
    expect(stats.errorFrames == 3, "events: and they are counted as error frames");
    expect(stats.rxFrames == 5, "events: against five received in total");

    std::filesystem::remove(path);
}

void test_receive_with_no_room()
{
    const auto path = write_temp("can_trc_backend_noroom.trc", kMultiBus);
    const Registry registry = registry_with(trc::ReplayOptions {});

    auto channel = registry.open("trc:" + path.string(), OpenOptions {});
    if (!channel.has_value())
    {
        expect(false, "no room: the channel opens");
        return;
    }

    // An empty span asks for nothing, so it gets nothing -- and must not
    // consume a frame on the way.
    auto none = (*channel)->receive(std::span<helpers::CanFrame> {}, Duration { 10 });
    expect(none.has_value() && *none == 0, "no room: an empty batch receives nothing");

    const std::vector<helpers::CanFrame> frames = drain(**channel, 5, Duration { 200 });
    expect(frames.size() == 5 && frames[0].id == 0x100,
           "no room: and no frame was lost to it");

    std::filesystem::remove(path);
}

// A path keeps its slashes because parse_channel_id only reads a trailing
// segment as a channel number when the whole segment is digits.
void test_channel_id_round_trip()
{
    auto id = parse_channel_id("trc:/var/log/run.trc");
    expect(id.has_value(), "channel id: an absolute path parses");
    if (id.has_value())
    {
        expect(id->backend == "trc" && id->device == "/var/log/run.trc" && id->channel == 0,
               "channel id: the whole path lands in device");
    }

    auto withBus = parse_channel_id("trc:/var/log/run.trc/2");
    expect(withBus.has_value(), "channel id: a path with a bus suffix parses");
    if (withBus.has_value())
    {
        expect(withBus->device == "/var/log/run.trc" && withBus->channel == 2,
               "channel id: and the trailing number is the bus, not part of the path");
    }
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);

    test_reads_every_bus();
    test_bus_filter();
    test_pacing();
    test_unpaced_is_immediate();
    test_stop_interrupts_a_paced_wait();
    test_loop_keeps_time_moving_forwards();
    test_send_is_counted_not_pretended();
    test_missing_file_fails_at_open();
    test_channel_interface();
    test_open_without_starting();
    test_enumerate_is_empty();
    test_no_bus_column();
    test_events_and_error_records();
    test_receive_with_no_room();
    test_channel_id_round_trip();

    if (failures == 0)
    {
        SPDLOG_INFO("can_trc backend tests passed");
        return EXIT_SUCCESS;
    }
    SPDLOG_ERROR("{} check(s) failed", failures);
    return EXIT_FAILURE;
}
