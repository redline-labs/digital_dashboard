// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AirPlay receiver: the RTSP server the phone connects to on port 7000 of
// the accessory's NCM link-local address, after CarPlayStartSession.
//
// Stage 7 of docs/carplay_bringup.md.
#ifndef AIRPLAY_RECEIVER_H_
#define AIRPLAY_RECEIVER_H_

#include "airplay/crypto.h"
#include "airplay/rtsp.h"
#include "airplay/timing.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace airplay
{

using Bytes = std::vector<uint8_t>;

struct ReceiverConfig
{
    // Address to bind. Empty binds to every interface, which is what the
    // bring-up wants; a link-local needs its scope id to bind specifically.
    std::string bind_address;
    uint16_t port = 7000;

    // Advertised in GET /info and used to derive pairing identity.
    std::string name = "Dashboard";
    std::string model = "MercedesDashboard1,1";

    // Screen geometry advertised to the phone.
    uint32_t width = 800;
    uint32_t height = 480;
    uint32_t fps = 30;

    // Advertised as both deviceID and macAddress in GET /info.
    std::string device_id = "02:00:00:00:00:01";

    // MFi coprocessor access for /auth-setup (MFiSAP). Left empty the receiver
    // answers 501, which stops the session: CarPlay will not proceed without a
    // genuine Apple authentication chip.
    std::function<Bytes()> mfi_certificate;
    std::function<Bytes(const Bytes& digest)> mfi_sign;
    // 2 => SHA-1/20-byte digests, 3 => SHA-256/32-byte.
    std::function<int()> mfi_protocol_major;
};

// Decoded media handed to the node for publishing on zenoh.
struct VideoPacket
{
    Bytes data;  // Annex-B
    uint64_t timestamp = 0;
    bool keyframe = false;
    // True for the codec parameter sets rather than a frame. zenoh has no
    // retained messages, so the node republishes these before every keyframe.
    bool is_config = false;
};

struct AudioPacket
{
    Bytes data;
    uint32_t sample_rate = 44100;
    uint8_t channels = 2;
};

class Receiver
{
  public:
    explicit Receiver(ReceiverConfig config);
    ~Receiver();

    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    using VideoHandler = std::function<void(const VideoPacket&)>;
    using AudioHandler = std::function<void(const AudioPacket&)>;

    void setVideoHandler(VideoHandler handler);
    void setAudioHandler(AudioHandler handler);

    bool start();
    void stop();
    bool running() const { return run_.load(); }

  private:
    void acceptLoop();
    void sessionLoop(int client_fd, std::string peer);

    // Dispatch for one parsed request. Returns the response to send.
    rtsp::Message handle(const rtsp::Message& request);

    rtsp::Message handlePairSetup(const rtsp::Message& request);
    rtsp::Message handlePairVerify(const rtsp::Message& request);
    rtsp::Message handleAuthSetup(const rtsp::Message& request);
    rtsp::Message handleInfo(const rtsp::Message& request);
    rtsp::Message handleSetup(const rtsp::Message& request);
    rtsp::Message handleRecord(const rtsp::Message& request);

    // Opens a listening TCP socket on an ephemeral port. Returns the fd and
    // writes the chosen port, which is what gets advertised to the phone.
    int openEphemeralListener(uint16_t& port);

    // Accepts the phone's video data connection and pumps frames until it
    // closes. `key` is the per-stream ChaCha20-Poly1305 key.
    void screenStreamLoop(int listen_fd, Bytes key);

    ReceiverConfig config_;
    VideoHandler video_handler_;
    AudioHandler audio_handler_;

    int server_fd_ = -1;
    std::atomic<bool> run_{false};
    std::thread accept_thread_;
    std::vector<std::thread> session_threads_;

    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace airplay

#endif  // AIRPLAY_RECEIVER_H_
