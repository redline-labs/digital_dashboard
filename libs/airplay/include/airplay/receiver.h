// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AirPlay receiver: the RTSP server the phone connects to on port 7000 of
// the accessory's NCM link-local address, after CarPlayStartSession.
//
// Stage 7 of docs/carplay_bringup.md.
#ifndef AIRPLAY_RECEIVER_H_
#define AIRPLAY_RECEIVER_H_

#include "airplay/config.h"
#include "airplay/crypto.h"
#include "airplay/hid.h"
#include "airplay/media_stream.h"
#include "airplay/nalu.h"
#include "airplay/oem_button.h"
#include "plist/value.h"
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

    // Called when the user presses the manufacturer button on CarPlay's home
    // screen -- the phone's way of saying "hand the screen back to the vehicle".
    // Runs on the event-channel thread, so do not block in it.
    using OemButtonHandler = std::function<void()>;

    void setVideoHandler(VideoHandler handler);
    void setAudioHandler(AudioHandler handler);
    void setStatusHandler(StatusHandler handler);
    void setOemButtonHandler(OemButtonHandler handler);

    // Which part of a gesture a touch report is. Down and Up both map to the
    // same single "contact down" bit on the wire, so this does not change the
    // HID report -- it exists because Move is the only phase that may be
    // coalesced when reports queue up. Collapsing a Down into a following Move
    // would move the press to where the finger ended up and turn a drag into a
    // tap somewhere else.
    enum class TouchPhase
    {
        Down,
        Move,
        Up,
    };

    // Injects a touch contact. x and y are normalised 0..1 over the screen.
    // Safe to call from any thread and never blocks: the report is queued for
    // the event-channel writer. Dropped if the channel is not up.
    void sendTouch(float x, float y, TouchPhase phase);

    // --- The non-touch input devices (see airplay/hid.h) --------------------
    //
    // All three queue rather than block, like sendTouch, and are dropped when
    // the event channel is not up. A head unit's physical controls arrive here.

    // Sends the knob's state. `momentary` follows it with an all-clear report,
    // which is what turns a press into a click and a turn into a single detent;
    // pass false to hold a button down until a later report releases it.
    void sendKnob(const hid::KnobState& state, bool momentary = true);

    // A media or telephony key press. Both are momentary: the key index goes
    // out followed by a release, because the phone acts on the transition.
    void sendMediaKey(hid::MediaKey key);
    void sendTelephonyKey(hid::TelephonyKey key);

    // Switches CarPlay's own UI between its day and night themes. Safe to call
    // before a session exists: the setting is remembered and pushed at RECORD,
    // which is also the earliest the phone will accept it -- a command sent
    // before RECORD is ignored, and stalls the bring-up on older iOS.
    void setNightMode(bool night);
    bool nightMode() const { return night_mode_.load(); }

    // Asks the phone to start Siri, as a dedicated Siri button rather than
    // push-to-talk: a down/up click, so the phone opens a conversational
    // session that listens until the user stops talking. A press-and-hold would
    // submit on release, cutting the user off before they had said anything.
    void requestSiri();

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

    rtsp::Message handleInfo(const rtsp::Message& request);
    rtsp::Message handleSetup(const rtsp::Message& request);
    rtsp::Message handleRecord(const rtsp::Message& request);
    rtsp::Message handleTeardown(const rtsp::Message& request);
    rtsp::Message handleFeedback(const rtsp::Message& request);

    // Reports the session as over, once, however it ended. Idempotent, because
    // a polite TEARDOWN and the control connection closing behind it are both
    // the same session ending.
    void endSession(const char* reason);

    // Brings a microphone uplink up (against the phone's dataPort) or down.
    void startMicUplink(uint16_t phone_port, const Bytes& shared_key, uint32_t sample_rate,
                        uint8_t channels, int stream_type, const plist::Value& stream);
    void stopMicUplink();

    // Routes one command the phone posted on the event channel. Returns the
    // response to send back; the phone expects every request acknowledged.
    rtsp::Message handleEventCommand(const rtsp::Message& request);

    // Encrypts and writes one plist command to the event channel socket.
    // Returns false if the channel is not up. Blocking, and called only from
    // eventSendLoop() -- everything else queues instead, so that no caller ends
    // up waiting on a socket write.

    // Encrypts and writes an already-framed RTSP message to the event channel:
    // the acknowledgements the receive pump owes the phone.

    // The shared body of both writers. Requires State::event_mutex, which is
    // what serialises the outbound nonce against concurrent senders.

    // Sole writer of the event channel. Drains queued touch reports and
    // keyframe requests; see State::send_queue for the ordering and coalescing
    // rules it enforces.

    // Builds the plist body for a touch report / keyframe request.

    // Queues one already-encoded event-channel command body for the sender
    // thread. Everything that is not touch or a keyframe goes through here.

    // Queues a HID report for one of the non-touch devices.

    ReceiverConfig config_;
    VideoHandler video_handler_;
    AudioHandler audio_handler_;
    StatusHandler status_handler_;
    MicStatusHandler mic_status_handler_;
    OemButtonHandler oem_button_handler_;

    int server_fd_ = -1;
    std::atomic<bool> run_{false};
    // True between RECORD and whatever ends the session. Atomic because the
    // RTSP session thread sets it and stop() reads it from the caller's.
    std::atomic<bool> session_live_{false};
    std::atomic<bool> night_mode_{false};
    // Whether Siri is listening or speaking, from the phone's modesChanged.
    // Only the event-channel thread touches it, so it needs no synchronisation;
    // it exists so the transition is logged once rather than on every update.
    bool speech_active_ = false;
    std::thread accept_thread_;
    std::thread keyframe_thread_;
    // steady_clock ticks (ns) of the last keyframe/config the phone sent. The
    // keyframe thread only nudges the phone when this goes stale, so an animated
    // screen (already emitting keyframes) is not asked for redundant ones.
    std::atomic<int64_t> last_keyframe_ns_{0};
    std::vector<std::thread> session_threads_;

    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace airplay

#endif  // AIRPLAY_RECEIVER_H_
