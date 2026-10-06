// SPDX-License-Identifier: GPL-3.0-or-later
//
// MuxHost over a fake pipe: what happens when a USB write fails.
//
// Every write in the mux happens on a thread with nothing above it to catch --
// the reader sending an ACK, a relay pump, connect()'s SYN -- so a write that
// threw took the whole node down. An unplug racing a write is enough. A failed
// write must instead take the mux down the way an unplug does: every stream
// released, alive() false, and no further writes attempted.
#include "apple_usb/muxd.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

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

constexpr uint32_t kProtoTcp = 6;
constexpr uint8_t kThSyn = 0x02;
constexpr uint8_t kThAck = 0x10;

void putBe16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}

void putBe32(std::vector<uint8_t>& v, uint32_t x)
{
    putBe16(v, static_cast<uint16_t>(x >> 16));
    putBe16(v, static_cast<uint16_t>(x));
}

uint16_t getBe16(const uint8_t* p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

// A TCP segment from the phone to the host's stream `sport`.
std::vector<uint8_t> phoneSegment(uint16_t sport, uint8_t flags, const std::string& payload = {})
{
    std::vector<uint8_t> tcp;
    putBe16(tcp, 62078);  // the phone's port
    putBe16(tcp, sport);  // ours
    putBe32(tcp, 1000);   // seq
    putBe32(tcp, 1);      // ack
    tcp.push_back(0x50);
    tcp.push_back(flags);
    putBe16(tcp, 512);
    putBe16(tcp, 0);
    putBe16(tcp, 0);
    tcp.insert(tcp.end(), payload.begin(), payload.end());

    std::vector<uint8_t> packet;
    putBe32(packet, kProtoTcp);
    putBe32(packet, static_cast<uint32_t>(16 + tcp.size()));
    putBe32(packet, 0xFEEDFACE);
    putBe16(packet, 0);
    putBe16(packet, 0);
    packet.insert(packet.end(), tcp.begin(), tcp.end());
    return packet;
}

// The phone's side of the pipe. Reads come from a queue the test fills; writes
// are recorded, and throw EIO once `failing` is set.
class FakePipe final : public apple_usb::MuxTransport
{
  public:
    void write(const uint8_t* data, size_t len) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failing)
        {
            ++refused;
            throw std::system_error(EIO, std::generic_category(), "fake bulk write");
        }
        written.emplace_back(data, data + len);
        cv_.notify_all();
    }

    std::vector<uint8_t> read(unsigned timeout_ms) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                          [this] { return !inbound_.empty(); }))
        {
            throw std::system_error(ETIMEDOUT, std::generic_category(), "fake bulk read");
        }
        std::vector<uint8_t> out = std::move(inbound_.front());
        inbound_.pop_front();
        return out;
    }

    void deliver(std::vector<uint8_t> bytes)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inbound_.push_back(std::move(bytes));
        cv_.notify_all();
    }

    // Waits for the host's SYN and returns the stream's source port, or 0.
    uint16_t awaitSyn()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        uint16_t sport = 0;
        cv_.wait_for(lock, std::chrono::seconds(2), [&] {
            for (const auto& packet : written)
            {
                if (packet.size() >= 36 && packet[3] == kProtoTcp && (packet[16 + 13] & kThSyn))
                {
                    sport = getBe16(packet.data() + 16);
                    return true;
                }
            }
            return false;
        });
        return sport;
    }

    size_t writes()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return written.size() + refused;
    }

    void setFailing(bool value)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        failing = value;
    }

  private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::vector<uint8_t>> inbound_;
    std::vector<std::vector<uint8_t>> written;
    size_t refused = 0;
    bool failing = false;
};

// An open host over a fake pipe; the version reply is queued so open() succeeds.
std::unique_ptr<apple_usb::MuxHost> openHost(FakePipe*& pipe)
{
    auto owned = std::make_unique<FakePipe>();
    pipe = owned.get();
    pipe->deliver({0, 0, 0, 0, 0, 0, 0, 20});
    auto host = std::make_unique<apple_usb::MuxHost>(std::move(owned));
    expect(host->open(), "the host opens over the fake pipe");
    expect(host->alive(), "and is alive");
    return host;
}

void testAckWriteFailsOnTheReader()
{
    FakePipe* pipe = nullptr;
    auto host = openHost(pipe);

    auto connecting = std::async(std::launch::async, [&] { return host->connect(62078); });
    const uint16_t sport = pipe->awaitSyn();
    expect(sport != 0, "connect sends a SYN");
    pipe->deliver(phoneSegment(sport, kThSyn | kThAck));
    const auto conn = connecting.get();
    expect(conn != nullptr, "the stream connects");
    if (conn == nullptr)
    {
        return;
    }

    // Data arrives, and the ACK the reader sends for it fails: the unplug
    // racing a write. This used to be std::terminate on the reader thread.
    pipe->setFailing(true);
    pipe->deliver(phoneSegment(sport, kThAck, "hello"));

    const auto first = conn->recv();
    expect(std::string(first.begin(), first.end()) == "hello",
           "the data that arrived is still delivered");
    expect(conn->recv().empty(), "then the stream reads EOF");
    expect(!host->alive(), "and the host reports itself dead");

    const size_t writes_before = pipe->writes();
    const uint8_t byte = 0x42;
    expect(!conn->send(&byte, 1), "a send after the failure reports it");
    expect(pipe->writes() == writes_before, "and does not touch the pipe again");
    expect(host->connect(62078) == nullptr, "nor can a new stream be opened");

    host->close();
}

void testSynWriteFails()
{
    FakePipe* pipe = nullptr;
    auto host = openHost(pipe);
    pipe->setFailing(true);

    const auto started = std::chrono::steady_clock::now();
    const auto conn = host->connect(62078);
    const auto took = std::chrono::steady_clock::now() - started;
    expect(conn == nullptr, "a SYN that cannot be written fails the connect");
    expect(took < std::chrono::seconds(1), "at once, rather than after the five-second wait");
    expect(!host->alive(), "and takes the host down");
    host->close();
}

}  // namespace

int main()
{
    testAckWriteFailsOnTheReader();
    testSynWriteFails();
    if (failures == 0)
    {
        SPDLOG_INFO("all mux transport-failure checks passed");
    }
    return failures == 0 ? 0 : 1;
}
