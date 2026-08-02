// SPDX-License-Identifier: GPL-3.0-or-later

#include "airplay/event_channel.h"

#include "airplay/crypto.h"
#include "airplay/net.h"
#include "plist/binary.h"
#include "plist/value.h"

#include <spdlog/spdlog.h>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <utility>

namespace airplay
{

EventChannel::EventChannel(Config config) : config_(std::move(config))
{
}

EventChannel::~EventChannel()
{
    stop();
}

void EventChannel::setCommandHandler(CommandHandler handler)
{
    command_handler_ = std::move(handler);
}

bool EventChannel::start(uint16_t& port)
{
    if (run_.load())
    {
        port = 0;
        return false;
    }
    listen_fd_ = net::openEphemeralListener(port);
    if (listen_fd_ < 0)
    {
        SPDLOG_ERROR("[airplay] could not open the event channel listener");
        return false;
    }

    run_.store(true);
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        send_stop_ = false;
    }
    accept_thread_ = std::thread([this] { acceptLoop(); });
    send_thread_ = std::thread([this] { sendLoop(); });
    return true;
}

void EventChannel::stop()
{
    if (!run_.exchange(false))
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        send_stop_ = true;
    }
    send_cv_.notify_all();
    if (send_thread_.joinable())
    {
        send_thread_.join();
    }
    if (accept_thread_.joinable())
    {
        accept_thread_.join();
    }
    // Closed only after the thread polling it has been joined, and reset so a
    // restarted channel opens a fresh one rather than advertising a port
    // nothing is listening on.
    if (listen_fd_ >= 0)
    {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void EventChannel::useSharedSecret(const Bytes& verify_shared)
{
    std::lock_guard<std::mutex> lock(socket_mutex_);
    shared_secret_ = verify_shared;
}

void EventChannel::clearQueue()
{
    std::lock_guard<std::mutex> lock(send_mutex_);
    send_queue_.clear();
}

void EventChannel::acceptLoop()
{
    while (run_.load())
    {
        pollfd pfd{listen_fd_, POLLIN, 0};
        if (::poll(&pfd, 1, 200) <= 0)
        {
            continue;
        }
        const int client = ::accept(listen_fd_, nullptr, nullptr);
        if (client < 0)
        {
            continue;
        }
        SPDLOG_INFO("[airplay] event channel connected");

        // Events keys are derived from the pair-verify shared secret and, unlike
        // the control channel, are NOT swapped: the accessory writes with
        // Events-Write and reads with Events-Read.
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            client_fd_ = client;
            // Not swapped, unlike the control channel: the accessory writes
            // with Events-Write and reads with Events-Read.
            crypto_.activate(
                crypto::hkdfSha512(shared_secret_, "Events-Salt",
                                   "Events-Read-Encryption-Key", 32),
                crypto::hkdfSha512(shared_secret_, "Events-Salt",
                                   "Events-Write-Encryption-Key", 32));
        }

        Bytes cipher;
        // Outlives the recv loop: a command can straddle TCP segments, and the
        // leftover of a partial one has to survive until the rest turns up.
        Bytes plain;
        Bytes chunk(8192);
        while (run_.load())
        {
            pollfd cpfd{client, POLLIN, 0};
            if (::poll(&cpfd, 1, 200) <= 0)
            {
                continue;
            }
            const ssize_t n = ::recv(client, chunk.data(), chunk.size(), 0);
            if (n <= 0)
            {
                break;
            }
            cipher.insert(cipher.end(), chunk.begin(), chunk.begin() + n);

            {
                std::lock_guard<std::mutex> lock(socket_mutex_);
                if (!crypto_.open(cipher, plain))
                {
                    SPDLOG_WARN("[airplay] event channel frame failed to authenticate");
                    break;
                }
            }

            // Drain every complete request the phone has pushed. Same framing as
            // the control channel: RTSP requests, binary-plist bodies.
            bool fatal = false;
            while (true)
            {
                rtsp::Message request;
                const auto consumed = rtsp::parseRequest(plain, request);
                if (!consumed)
                {
                    SPDLOG_WARN("[airplay] malformed event-channel request; closing");
                    fatal = true;
                    break;
                }
                if (*consumed == 0)
                {
                    break;  // need more bytes
                }
                plain.erase(plain.begin(), plain.begin() + static_cast<long>(*consumed));

                if (request.isResponse())
                {
                    // The phone acknowledging something we sent. Consumed and
                    // dropped: answering it makes it answer us back, forever.
                    SPDLOG_DEBUG("[airplay] event channel: reply to our command ({} {})",
                                 request.method, request.uri);
                    continue;
                }

                rtsp::Message response = command_handler_ ? command_handler_(request)
                                             : rtsp::makeResponse(200, "OK", "", {});
                // The phone retries a command it never sees acknowledged, so an
                // unanswered channel looks like a stall rather than a silent drop.
                if (const std::string* cseq = request.header("CSeq"); cseq != nullptr)
                {
                    response.setHeader("CSeq", *cseq);
                }
                response.setHeader("Server", "AirTunes/366.0");
                if (!writeRaw(rtsp::serializeResponse(response)))
                {
                    fatal = true;
                    break;
                }
            }
            if (fatal)
            {
                break;
            }
        }

        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            client_fd_ = -1;
            crypto_.deactivate();
        }
        // Anything still queued belongs to the session that just ended -- most
        // likely a touch whose matching release never got sent. Replaying it
        // into the next session would inject a phantom contact.
        {
            std::lock_guard<std::mutex> lock(send_mutex_);
            send_queue_.clear();
        }
        ::close(client);
        SPDLOG_INFO("[airplay] event channel closed");
    }
}
Bytes EventChannel::buildTouchCommand(float x, float y, bool down) const
{
    // The descriptor reports absolute coordinates, so the caller's normalised
    // 0..1 is scaled to the display it was advertised against. We only ever
    // drive contact 0; the second slot is reported empty.
    const auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    hid::Contact contact;
    contact.x = static_cast<uint16_t>(clamp01(x) * static_cast<float>(config_.width));
    contact.y = static_cast<uint16_t>(clamp01(y) * static_cast<float>(config_.height));
    contact.down = down;

    return hid::sendReportCommand(hid::kTouchUid, hid::touchReport({contact}));
}
Bytes EventChannel::buildKeyframeCommand() const
{
    // forceKeyFrame names the display by the same uuid advertised in /info.
    plist::Value params = plist::Value::dict();
    params.set("uuid", plist::Value::string(config_.display_uuid));
    plist::Value command = plist::Value::dict();
    command.set("type", plist::Value::string("forceKeyFrame"));
    command.set("params", std::move(params));
    return plist::encodeBinary(command);
}
Bytes EventChannel::buildNightModeCommand() const
{
    plist::Value params = plist::Value::dict();
    params.set("nightMode", plist::Value::boolean(night_mode_.load()));
    plist::Value command = plist::Value::dict();
    command.set("type", plist::Value::string("setNightMode"));
    command.set("params", std::move(params));
    return plist::encodeBinary(command);
}
void EventChannel::sendTouch(float x, float y, TouchPhase phase)
{
    EventQueue::TouchReport report;
    report.x = x;
    report.y = y;
    report.down = (phase != TouchPhase::Up);
    report.coalescable = (phase == TouchPhase::Move);

    bool dropped = false;
    uint64_t drop_count = 0;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        dropped = !send_queue_.pushTouch(report);
        drop_count = send_queue_.dropped();
    }
    if (dropped)
    {
        // Means the link itself has stalled, so it is loud -- but rate limited,
        // since whatever caused it will not have caused it just once.
        if (drop_count % 100 == 1)
        {
            SPDLOG_WARN("[airplay] event channel backed up; dropped {} touch report(s)",
                        drop_count);
        }
        return;
    }
    send_cv_.notify_one();
}
void EventChannel::queueCommand(Bytes body)
{
    bool dropped = false;
    uint64_t drop_count = 0;
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        dropped = !send_queue_.pushControl(std::move(body));
        drop_count = send_queue_.dropped();
    }
    if (dropped)
    {
        // Unlike a dropped touch move this loses a discrete user action, so it
        // is worth a line every time rather than a sampled one.
        SPDLOG_WARN("[airplay] event channel backed up; dropped a control command "
                    "({} event(s) dropped so far)",
                    drop_count);
        return;
    }
    send_cv_.notify_one();
}
void EventChannel::queueReport(uint32_t uid, const Bytes& report)
{
    queueCommand(hid::sendReportCommand(uid, report));
}
void EventChannel::sendKnob(const hid::KnobState& state, bool momentary)
{
    queueReport(hid::kKnobUid, hid::knobReport(state));
    if (momentary)
    {
        // The buttons are levels: without this the phone goes on believing the
        // button is held. The wheel and pointer are relative, so the all-clear
        // report is a no-op for them and costs only a message.
        queueReport(hid::kKnobUid, hid::knobReport({}));
    }
}
void EventChannel::sendMediaKey(hid::MediaKey key)
{
    queueReport(hid::kMediaUid, hid::mediaReport(key));
    queueReport(hid::kMediaUid, hid::mediaReport(hid::MediaKey::None));
}
void EventChannel::sendTelephonyKey(hid::TelephonyKey key)
{
    queueReport(hid::kTelephonyUid, hid::telephonyReport(key));
    queueReport(hid::kTelephonyUid, hid::telephonyReport(hid::TelephonyKey::None));
}
void EventChannel::setNightMode(bool night)
{
    night_mode_.store(night);
}

void EventChannel::pushNightMode()
{
    SPDLOG_INFO("[airplay] night mode {}", night_mode_.load() ? "on" : "off");
    queueCommand(buildNightModeCommand());
}
void EventChannel::requestSiri()
{
    // siriAction 2 is button-down, 3 button-up. Sent back to back so the phone
    // sees a click: it then listens until the user stops speaking, rather than
    // treating the release as "done talking" and cutting them off.
    constexpr int64_t kSiriButtonDown = 2;
    constexpr int64_t kSiriButtonUp = 3;

    const auto build = [](int64_t action) {
        plist::Value params = plist::Value::dict();
        params.set("siriAction", plist::Value::integer(action));
        plist::Value command = plist::Value::dict();
        command.set("type", plist::Value::string("requestSiri"));
        command.set("params", std::move(params));
        return plist::encodeBinary(command);
    };

    SPDLOG_INFO("[airplay] Siri requested");
    queueCommand(build(kSiriButtonDown));
    queueCommand(build(kSiriButtonUp));
}
void EventChannel::requestKeyframe()
{
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        send_queue_.requestKeyframe();
    }
    send_cv_.notify_one();
}
void EventChannel::sendLoop()
{
    // Threading and I/O only -- what to send, in what order, and what to drop
    // is EventQueue's business, and is tested directly in test_event_queue.cpp.
    while (true)
    {
        std::unique_lock<std::mutex> lock(send_mutex_);
        send_cv_.wait(lock,
                             [this] { return send_stop_ || send_queue_.hasWork(); });
        if (send_stop_)
        {
            return;
        }

        const auto next = send_queue_.take(std::chrono::steady_clock::now());
        if (next.action == EventQueue::Action::WaitTouch)
        {
            // Nothing was consumed. Waiting here rather than sending is what
            // lets a move arriving meanwhile coalesce, and lets a keyframe
            // request landing during the wait overtake the queued touch.
            send_cv_.wait_for(lock, next.wait);
            continue;
        }
        lock.unlock();

        switch (next.action)
        {
            case EventQueue::Action::SendKeyframe:
                writeCommand(buildKeyframeCommand());
                break;
            case EventQueue::Action::SendControl:
                writeCommand(next.control);
                break;
            case EventQueue::Action::SendTouch:
                writeCommand(
                    buildTouchCommand(next.touch.x, next.touch.y, next.touch.down));
                break;
            case EventQueue::Action::Idle:
                break;  // spurious wake, just go round again
            case EventQueue::Action::WaitTouch:
                break;  // handled above, before the lock was released
        }
    }
}
bool EventChannel::writeCommand(const Bytes& plist_body)
{
    // The CSeq counter and the outbound nonce advance together, under one lock:
    // taking them separately would let two writers number their messages in one
    // order and encrypt them in the other, which the phone reads as a replay.
    std::lock_guard<std::mutex> lock(socket_mutex_);

    const std::string head = "POST /command RTSP/1.0\r\n"
                             "Content-Type: application/x-apple-binary-plist\r\n"
                             "Content-Length: " +
                             std::to_string(plist_body.size()) +
                             "\r\nCSeq: " + std::to_string(++cseq_) + "\r\n\r\n";

    Bytes message(head.begin(), head.end());
    message.insert(message.end(), plist_body.begin(), plist_body.end());
    return writeLocked(message);
}
bool EventChannel::writeRaw(const Bytes& message)
{
    std::lock_guard<std::mutex> lock(socket_mutex_);
    return writeLocked(message);
}
bool EventChannel::writeLocked(const Bytes& message)
{
    if (client_fd_ < 0 || !crypto_.active())
    {
        return false;  // no event channel yet
    }

    const Bytes wire = crypto_.seal(message);
    size_t sent = 0;
    while (sent < wire.size())
    {
        const ssize_t written = ::send(client_fd_, wire.data() + sent,
                                       wire.size() - sent, MSG_NOSIGNAL);
        if (written <= 0)
        {
            SPDLOG_DEBUG("[airplay] event channel send failed: {}", std::strerror(errno));
            return false;
        }
        sent += static_cast<size_t>(written);
    }
    return true;
}
}  // namespace airplay
