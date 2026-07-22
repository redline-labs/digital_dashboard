// SPDX-License-Identifier: GPL-3.0-or-later
//
// CarPlay clock sync on the timing port (UDP, RTCP-style NTP).
//
// The receiver drives this: it sends type-210 requests to the phone's timing
// port and steers a local clock from the replies. It is not optional -- without
// it the phone tears the session down a few seconds after RECORD.
#ifndef AIRPLAY_TIMING_H_
#define AIRPLAY_TIMING_H_

#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>

namespace airplay
{

class TimingSync
{
  public:
    TimingSync() = default;
    ~TimingSync();

    TimingSync(const TimingSync&) = delete;
    TimingSync& operator=(const TimingSync&) = delete;

    // Binds a dual-stack UDP socket and reports the port to advertise.
    bool listen(uint16_t& port);

    // Starts periodic requests to the phone's timing port. The scope id is
    // required for link-local peers.
    void start(const std::string& peer_host, uint16_t peer_port, uint32_t scope_id);

    void stop();

    // Now, in the phone's clock domain, as a 64-bit NTP value.
    uint64_t syncedNtp() const;

    bool synced() const { return synced_; }

  private:
    void loop();
    void sendRequest();
    void handlePacket(const uint8_t* data, size_t length, const sockaddr_in6& from);

    int fd_ = -1;
    uint16_t port_ = 0;
    sockaddr_in6 peer_{};

    std::atomic<bool> run_{false};
    std::thread thread_;

    int64_t clock_offset_ns_ = 0;
    uint64_t pending_t1_ = 0;
    bool synced_ = false;
};

}  // namespace airplay

#endif  // AIRPLAY_TIMING_H_
