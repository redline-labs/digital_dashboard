// SPDX-License-Identifier: GPL-3.0-or-later
//
// The event channel: the second TCP connection of an AirPlay session, and the
// only path for anything that is not media.
//
// Everything the user does travels out over it -- touch, the rotary controller,
// media and phone keys, Siri -- along with the accessory's own requests
// (forceKeyFrame, setNightMode). It is bidirectional: the phone pushes its own
// commands back the other way, and expects each one acknowledged.
//
// The phone dials it at the port advertised in the session SETUP response, and
// it is encrypted from its first byte with keys derived from the pair-verify
// shared secret.
//
// Threading. One thread accepts and pumps the socket; one thread drains the
// outbound queue. Nothing else touches the socket, and no producer ever blocks
// on it: sendTouch() runs on the zenoh subscriber thread and requestKeyframe()
// on a timer thread, and neither may stall behind a congested send(). What is
// sent, in what order, and what is dropped is EventQueue's business.
#ifndef AIRPLAY_EVENT_CHANNEL_H_
#define AIRPLAY_EVENT_CHANNEL_H_

#include "airplay/channel_crypto.h"
#include "airplay/event_queue.h"
#include "airplay/hid.h"
#include "airplay/rtsp.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace airplay
{

class EventChannel
{
  public:
    struct Config
    {
        // The display touch coordinates are scaled to. The HID descriptor
        // advertises absolute coordinates, so these must match what GET /info
        // declared or every touch lands in the wrong place.
        uint32_t width = 800;
        uint32_t height = 600;
        // The display keyframe requests are aimed at.
        std::string display_uuid;
    };

    // Routes one command the phone pushed, and returns the response to send
    // back. Called on the channel's receive thread.
    using CommandHandler = std::function<rtsp::Message(const rtsp::Message&)>;

    explicit EventChannel(Config config);
    ~EventChannel();

    EventChannel(const EventChannel&) = delete;
    EventChannel& operator=(const EventChannel&) = delete;

    void setCommandHandler(CommandHandler handler);

    // Opens the listening socket and starts both threads. Writes the port the
    // SETUP response should advertise. The phone dials it as soon as it has the
    // port, so this has to be running before that response goes out -- a
    // connection left unaccepted in the backlog reads as a dead channel.
    bool start(uint16_t& port);
    void stop();

    // Installs the keys, derived from the pair-verify shared secret. Unlike the
    // control channel these are *not* swapped: the accessory writes with
    // Events-Write and reads with Events-Read.
    void useSharedSecret(const Bytes& verify_shared);

    // --- Outbound -----------------------------------------------------------
    // All of these queue and return immediately, and are safe from any thread.
    // Anything queued while no phone is connected is dropped.

    enum class TouchPhase
    {
        Down,
        Move,
        Up,
    };
    // x and y are normalised 0..1 over the display.
    void sendTouch(float x, float y, TouchPhase phase);

    void sendKnob(const hid::KnobState& state, bool momentary = true);
    void sendMediaKey(hid::MediaKey key);
    void sendTelephonyKey(hid::TelephonyKey key);
    void requestSiri();

    // Records which theme CarPlay should draw. Split from pushNightMode()
    // because *when* to send it is the session's business, not the channel's:
    // the phone ignores an event command sent before RECORD, and older iOS
    // stalls the bring-up for seconds on one.
    void setNightMode(bool night);
    void pushNightMode();

    // Asks the phone for a fresh IDR. The phone sends one keyframe at session
    // start and then, for a static screen, only P-frames -- so a renderer that
    // joins late (the dashboard does, over zenoh) cannot sync without this.
    void requestKeyframe();

    // Queues an already-encoded command body. The escape hatch for anything not
    // covered above.
    void queueCommand(Bytes body);

    // Drops everything queued. Called when a session ends: what is pending
    // belongs to that session -- most likely a touch whose matching release
    // never got sent -- and replaying it would inject a phantom contact.
    void clearQueue();

  private:
    void acceptLoop();
    void sendLoop();

    // Encrypts and writes one already-framed RTSP message. Takes the mutex that
    // serialises the outbound nonce.
    bool writeRaw(const Bytes& message);
    // Wraps a plist body in the POST /command the channel carries.
    bool writeCommand(const Bytes& plist_body);
    bool writeLocked(const Bytes& message);

    void queueReport(uint32_t uid, const Bytes& report);
    Bytes buildTouchCommand(float x, float y, bool down) const;
    Bytes buildKeyframeCommand() const;
    Bytes buildNightModeCommand() const;

    Config config_;
    CommandHandler command_handler_;

    std::atomic<bool> run_{false};
    std::atomic<bool> night_mode_{false};
    int listen_fd_ = -1;
    std::thread accept_thread_;
    std::thread send_thread_;

    // The accepted socket and its cipher. Guarded because the receive pump and
    // the send loop both write to it.
    std::mutex socket_mutex_;
    int client_fd_ = -1;
    ChannelCrypto crypto_;
    int cseq_ = 0;
    Bytes shared_secret_;

    // Outbound work. EventQueue is not thread safe; this mutex is what makes
    // it so.
    std::mutex send_mutex_;
    std::condition_variable send_cv_;
    EventQueue send_queue_;
    bool send_stop_ = false;
};

}  // namespace airplay

#endif  // AIRPLAY_EVENT_CHANNEL_H_
