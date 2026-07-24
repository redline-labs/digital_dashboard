// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AirPlay receiver: the RTSP server the phone connects to on port 7000 of
// the accessory's NCM link-local address, after CarPlayStartSession.
//
// Stage 7 of docs/carplay_bringup.md.
#ifndef AIRPLAY_RECEIVER_H_
#define AIRPLAY_RECEIVER_H_

#include "airplay/crypto.h"
#include "airplay/plist.h"
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

    // Screen geometry advertised to the phone. Defaults match the carplay_demo
    // dashboard widget so the phone renders at the widget's aspect ratio.
    uint32_t width = 800;
    uint32_t height = 600;
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
    Bytes data;  // interleaved S16LE PCM
    uint32_t sample_rate = 44100;
    uint8_t channels = 2;
    // The CarPlay stream type (100 main, 101 alt, 102 entertainment) and the
    // audioType category ("media", "telephony", "speechRecognition", ...) so the
    // node can label the zenoh audio stream.
    int stream_type = 100;
    std::string audio_type;
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
    // Called with true when the session reaches RECORD (video about to flow) and
    // false when it tears down, so the node can publish an accurate session
    // state to the dashboard.
    using StatusHandler = std::function<void(bool recording)>;

    void setVideoHandler(VideoHandler handler);
    void setAudioHandler(AudioHandler handler);
    void setStatusHandler(StatusHandler handler);

    // Injects a touch contact. x and y are normalised 0..1 over the screen.
    // Safe to call from any thread; a no-op until the event channel is up.
    void sendTouch(float x, float y, bool down);

    // Called when the phone opens (active=true) or closes a microphone uplink,
    // with the sample rate and channel count it expects. The node uses this to
    // tell the dashboard widget to start/stop capturing.
    using MicStatusHandler = std::function<void(bool active, uint32_t sample_rate,
                                                uint8_t channels)>;
    void setMicStatusHandler(MicStatusHandler handler);

    // Feeds captured microphone PCM (S16LE interleaved, at the rate/channels the
    // phone requested) up to the phone. A no-op when no uplink is active. Safe
    // to call from any thread.
    void feedMic(const Bytes& pcm);

    // Asks the phone to emit a fresh IDR keyframe. The phone sends one keyframe
    // at session start and then, for a static screen, only P-frames -- so a
    // renderer that joins late (the dashboard does, over zenoh) cannot sync
    // without this. Called periodically while a session is live.
    void requestKeyframe();

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

    // Receives one CarPlay audio stream on its UDP data port: RTP-framed,
    // ChaCha20-Poly1305 sealed. Each decrypted payload is either LPCM (big
    // endian) or, when `is_aac`, a raw AAC-LC access unit decoded to PCM.
    void audioStreamLoop(int data_fd, Bytes key, uint32_t sample_rate, uint8_t channels,
                         int stream_type, std::string audio_type, bool is_aac);

    // Binds a dual-stack UDP socket on an ephemeral port. Returns the fd and
    // writes the chosen port.
    int openUdpSocket(uint16_t& port);

    // Brings a microphone uplink up (against the phone's dataPort) or down.
    void startMicUplink(uint16_t phone_port, const Bytes& shared_key, uint32_t sample_rate,
                        uint8_t channels, int stream_type, const plist::Value& stream);
    void stopMicUplink();

    // Accepts and services the phone's encrypted event-channel connection, over
    // which HID reports (touch) are pushed to the phone.
    void eventChannelLoop(int listen_fd);

    // Encrypts and sends one plist command over the event channel. Returns
    // false if the channel is not up.
    bool sendEventCommand(const Bytes& plist_body);

    ReceiverConfig config_;
    VideoHandler video_handler_;
    AudioHandler audio_handler_;
    StatusHandler status_handler_;
    MicStatusHandler mic_status_handler_;

    int server_fd_ = -1;
    std::atomic<bool> run_{false};
    std::thread accept_thread_;
    std::thread keyframe_thread_;
    std::vector<std::thread> session_threads_;

    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace airplay

#endif  // AIRPLAY_RECEIVER_H_
