// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/link_layer.py
#include "iap2/link_layer.h"

#include <spdlog/spdlog.h>

#include <algorithm>

#include "iap2/messages.h"

namespace iap2
{

namespace
{

// Bytes pulled from the transport per recv() call.
constexpr size_t kRecvChunkSize = 8192;

// Marker / SYN retransmission cadence (LIVI: call_later(1) and call_later(0.5)).
constexpr unsigned kMarkerResendMs = 1000;
constexpr unsigned kNegotiateResendMs = 500;

// Never let the control session buffer grow without bound if the peer sends
// garbage that never resynchronises to a CSM start marker.
constexpr size_t kMaxControlBuffer = 1 << 20;

void put_be16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

uint16_t get_be16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

const char* stateName(LinkLayer::State state)
{
    switch (state)
    {
        case LinkLayer::State::kIdle: return "idle";
        case LinkLayer::State::kDetectIap2Support: return "detect";
        case LinkLayer::State::kNegotiate: return "negotiate";
        case LinkLayer::State::kNormal: return "normal";
        case LinkLayer::State::kDead: return "dead";
    }
    return "?";
}

std::string controlName(uint8_t control)
{
    std::string name;
    if (control & kControlSyn)
    {
        name += "SYN|";
    }
    if (control & kControlAck)
    {
        name += "ACK|";
    }
    if (control & kControlEak)
    {
        name += "EAK|";
    }
    if (control & kControlRst)
    {
        name += "RST|";
    }
    if (name.empty())
    {
        return "DATA";
    }
    name.pop_back();
    return name;
}

}  // namespace

uint8_t genChecksum(const uint8_t* data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i)
    {
        sum = static_cast<uint8_t>(sum + data[i]);
    }
    return static_cast<uint8_t>(-static_cast<int>(sum) & 0xFF);
}

bool checkChecksum(const uint8_t* data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i)
    {
        sum = static_cast<uint8_t>(sum + data[i]);
    }
    return sum == 0;
}

uint8_t sequenceDistance(uint8_t a, int b)
{
    if (b < 0)
    {
        return 0;
    }
    const int value = (a >= b) ? (a - b) : (a + 256 - b);
    return static_cast<uint8_t>(value);
}

// ---------------------------------------------------------------------------
// LinkPacketHeader
// ---------------------------------------------------------------------------

std::vector<uint8_t> LinkPacketHeader::pack() const
{
    std::vector<uint8_t> out;
    out.reserve(kLinkHeaderSize);
    put_be16(out, kLinkPacketStart);
    put_be16(out, length);
    out.push_back(control);
    out.push_back(seq);
    out.push_back(ack);
    out.push_back(session_id);
    out.push_back(genChecksum(out.data(), out.size()));
    return out;
}

std::optional<LinkPacketHeader> LinkPacketHeader::parse(const uint8_t* data, size_t len)
{
    if (len < kLinkHeaderSize)
    {
        return std::nullopt;
    }
    if (get_be16(data) != kLinkPacketStart)
    {
        return std::nullopt;
    }
    if (!checkChecksum(data, kLinkHeaderSize))
    {
        return std::nullopt;
    }

    LinkPacketHeader header;
    header.length = get_be16(data + 2);
    header.control = data[4];
    header.seq = data[5];
    header.ack = data[6];
    header.session_id = data[7];
    return header;
}

// ---------------------------------------------------------------------------
// LinkSynchronizationPayload
// ---------------------------------------------------------------------------

std::vector<uint8_t> LinkSynchronizationPayload::pack() const
{
    std::vector<uint8_t> out;
    out.reserve(kFixedSize + sessions.size() * 3);
    out.push_back(kVersion);
    out.push_back(max_outgoing);
    put_be16(out, max_len);
    put_be16(out, retransmission_timeout);
    put_be16(out, ack_timeout);
    out.push_back(max_retransmissions);
    out.push_back(max_ack);
    for (const LinkSession& session : sessions)
    {
        out.push_back(session.id);
        out.push_back(session.type);
        out.push_back(session.version);
    }
    return out;
}

std::optional<LinkSynchronizationPayload> LinkSynchronizationPayload::parse(const uint8_t* data, size_t len)
{
    if (len < kFixedSize)
    {
        SPDLOG_WARN("[iap2] link synchronization payload is only {} bytes, need {}", len, kFixedSize);
        return std::nullopt;
    }
    if (data[0] != kVersion)
    {
        SPDLOG_WARN("[iap2] link synchronization payload version {} is not supported (expected {})", data[0],
                    kVersion);
        return std::nullopt;
    }

    LinkSynchronizationPayload lsp;
    lsp.max_outgoing = data[1];
    lsp.max_len = get_be16(data + 2);
    lsp.retransmission_timeout = get_be16(data + 4);
    lsp.ack_timeout = get_be16(data + 6);
    lsp.max_retransmissions = data[8];
    lsp.max_ack = data[9];
    for (size_t offset = kFixedSize; offset + 3 <= len; offset += 3)
    {
        lsp.sessions.push_back(LinkSession{data[offset], data[offset + 1], data[offset + 2]});
    }
    if ((len - kFixedSize) % 3 != 0)
    {
        SPDLOG_WARN("[iap2] link synchronization payload has {} trailing session bytes",
                    (len - kFixedSize) % 3);
    }
    return lsp;
}

// ---------------------------------------------------------------------------
// LinkLayer
// ---------------------------------------------------------------------------

LinkLayer::LinkLayer(Iap2Transport& transport, LinkConfig config)
    : transport_(transport), config_(std::move(config))
{
    lsp_.max_outgoing = config_.max_outgoing;
    lsp_.max_len = 65535;
    lsp_.retransmission_timeout = config_.zero_ack ? 0 : 4000;
    lsp_.ack_timeout = config_.zero_ack ? 0 : config_.ack_timeout_ms;
    lsp_.max_retransmissions = config_.zero_ack ? 0 : 4;
    lsp_.max_ack = config_.zero_ack ? 0 : 3;
    lsp_.sessions = {
        LinkSession{kControlSessionId, 0, config_.control_version},
        LinkSession{kExternalAccessorySessionId, 2, 1},
        LinkSession{kFileTransferSessionId, 1, 2},
    };
}

void LinkLayer::setControlMessageHandler(ControlMessageHandler handler)
{
    control_handler_ = std::move(handler);
}

void LinkLayer::setFileTransferHandler(FileTransferHandler handler)
{
    file_transfer_handler_ = std::move(handler);
}

void LinkLayer::setExternalAccessoryHandler(ExternalAccessoryHandler handler)
{
    external_accessory_handler_ = std::move(handler);
}

size_t LinkLayer::maxPayload() const
{
    // max_len is the maximum total packet length: 9 header bytes + payload +
    // 1 payload checksum byte.
    //
    // NOTE: LIVI chunks at max_len itself. That only works because with
    // initiate_negotiate the accessory keeps its own 65535 and never sends a
    // packet anywhere near that big. We subtract the framing overhead so a
    // device-supplied max_len is honoured exactly.
    constexpr size_t kOverhead = kLinkHeaderSize + 1;
    constexpr size_t kAbsoluteMax = 0xFFFF - kOverhead;
    if (lsp_.max_len <= kOverhead)
    {
        return 1;
    }
    return std::min<size_t>(static_cast<size_t>(lsp_.max_len) - kOverhead, kAbsoluteMax);
}

bool LinkLayer::start()
{
    if (state_ != State::kIdle)
    {
        SPDLOG_WARN("[iap2] start() called while link is already in state {}", stateName(state_));
        return false;
    }

    state_ = State::kDetectIap2Support;
    SPDLOG_DEBUG("[iap2] {}: state -> detect, sending iAP2 detection marker", config_.tag);
    sendDetectMarker();
    if (state_ == State::kDead)
    {
        return false;
    }

    if (config_.initiate_negotiate)
    {
        marker_deadline_.reset();
        state_ = State::kNegotiate;
        SPDLOG_DEBUG("[iap2] {}: state -> negotiate (initiating), max_outgoing={} control_version={} "
                     "zero_ack={}",
                     config_.tag, lsp_.max_outgoing, config_.control_version, config_.zero_ack);
        sendNegotiate();
    }
    return state_ != State::kDead;
}

void LinkLayer::close()
{
    if (state_ == State::kDead)
    {
        return;
    }
    SPDLOG_DEBUG("[iap2] {}: closing link (state was {})", config_.tag, stateName(state_));
    bailout(nullptr);
}

void LinkLayer::bailout(const char* reason)
{
    if (state_ == State::kDead)
    {
        return;
    }
    disarmSendAckTimer();
    disarmRecvAckTimer();
    marker_deadline_.reset();
    negotiate_deadline_.reset();
    state_ = State::kDead;
    if (reason != nullptr)
    {
        SPDLOG_ERROR("[iap2] {}: link is dead: {}", config_.tag, reason);
    }
}

void LinkLayer::writePacket(const uint8_t* payload, size_t payload_len, uint8_t seq, uint8_t control,
                            uint8_t session_id)
{
    cumulative_received_ = 0;

    LinkPacketHeader header;
    header.length = (payload_len > 0) ? static_cast<uint16_t>(payload_len + kLinkHeaderSize + 1)
                                      : static_cast<uint16_t>(kLinkHeaderSize);
    header.control = control;
    header.seq = seq;
    header.ack = last_received_in_sequence_psn_;
    header.session_id = session_id;

    std::vector<uint8_t> frame = header.pack();
    if (payload_len > 0)
    {
        frame.insert(frame.end(), payload, payload + payload_len);
        frame.push_back(genChecksum(payload, payload_len));
    }

    SPDLOG_DEBUG("[iap2] {} > {} seq={} ack={} session={} len={}", config_.tag, controlName(control), seq,
                 header.ack, session_id, header.length);

    if (!transport_.send(frame.data(), frame.size()))
    {
        bailout("transport write failed");
    }
}

void LinkLayer::sendAck() { writePacket(nullptr, 0, sent_psn_, kControlAck, 0); }

void LinkLayer::sendEak(const std::vector<uint8_t>& psns)
{
    SPDLOG_WARN("[iap2] {}: sending EAK for {} missing packet(s)", config_.tag, psns.size());
    writePacket(psns.data(), psns.size(), sent_psn_, kControlEak, 0);
}

void LinkLayer::sendData(const OutPacket& packet)
{
    writePacket(packet.data.data(), packet.data.size(), packet.psn, kControlAck, packet.session_id);
}

void LinkLayer::sendDetectMarker()
{
    if (state_ != State::kDetectIap2Support)
    {
        return;
    }
    if (!transport_.send(kIap2Marker, kIap2MarkerSize))
    {
        bailout("transport write failed sending the iAP2 detection marker");
        return;
    }
    marker_deadline_ = Clock::now() + std::chrono::milliseconds(kMarkerResendMs);
}

void LinkLayer::sendNegotiate()
{
    if (state_ != State::kNegotiate)
    {
        return;
    }
    const std::vector<uint8_t> payload = lsp_.pack();
    writePacket(payload.data(), payload.size(), sent_psn_, kControlSyn, 0);
    negotiate_deadline_ = Clock::now() + std::chrono::milliseconds(kNegotiateResendMs);
}

bool LinkLayer::sendSessionData(uint8_t session_id, const uint8_t* data, size_t len)
{
    if (state_ == State::kDead)
    {
        SPDLOG_ERROR("[iap2] {}: refusing to send {} bytes on session {}, link is dead", config_.tag, len,
                     session_id);
        return false;
    }

    if (len == 0)
    {
        SPDLOG_WARN("[iap2] {}: refusing to send an empty payload on session {}", config_.tag, session_id);
        return false;
    }

    const size_t chunk_size = maxPayload();
    size_t offset = 0;
    do
    {
        const size_t take = std::min(chunk_size, len - offset);
        OutPacket packet;
        packet.session_id = session_id;
        packet.data.assign(data + offset, data + offset + take);
        if (len > chunk_size)
        {
            SPDLOG_DEBUG("[iap2] {}: fragmenting session {} payload, {}/{} bytes", config_.tag, session_id,
                         offset + take, len);
        }
        enqueuePacket(std::move(packet));
        offset += take;
    } while (offset < len);

    return state_ != State::kDead;
}

void LinkLayer::enqueuePacket(OutPacket packet)
{
    if (state_ != State::kNormal ||
        sequenceDistance(sent_psn_, last_sent_acknowledged_psn_) > lsp_.max_outgoing)
    {
        SPDLOG_DEBUG("[iap2] {}: queuing {} byte packet for session {} (state={}, {} already queued)",
                     config_.tag, packet.data.size(), packet.session_id, stateName(state_), queued_.size());
        queued_.push_back(std::move(packet));
        return;
    }

    sent_psn_ = static_cast<uint8_t>(sent_psn_ + 1);
    packet.psn = sent_psn_;
    packet.counter = 0;
    packet.timeout = Clock::now() + std::chrono::milliseconds(lsp_.retransmission_timeout);

    disarmSendAckTimer();
    sendData(packet);
    last_acked_psn_ = last_received_in_sequence_psn_;

    if (lsp_.max_retransmissions > 0)
    {
        rearmRecvAckTimer(packet.timeout);
        unacked_.push_back(std::move(packet));
    }
    else
    {
        last_sent_acknowledged_psn_ = sent_psn_;
    }
}

bool LinkLayer::sendControlMessage(const std::vector<uint8_t>& message)
{
    const auto parsed = csm::peekLength(message.data(), message.size());
    if (!parsed || *parsed == 0 || *parsed != message.size())
    {
        SPDLOG_WARN("[iap2] {}: sending a {} byte control payload that is not a well formed CSM frame",
                    config_.tag, message.size());
    }
    else
    {
        const uint16_t msg_id = get_be16(message.data() + 4);
        SPDLOG_DEBUG("[iap2] {}: sending control message 0x{:04X} ({}), {} bytes", config_.tag, msg_id,
                     messageIdName(msg_id), message.size());
    }
    return sendSessionData(kControlSessionId, message.data(), message.size());
}

bool LinkLayer::sendFileTransfer(const std::vector<uint8_t>& data)
{
    return sendSessionData(kFileTransferSessionId, data.data(), data.size());
}

bool LinkLayer::sendExternalAccessory(uint16_t stream_id, const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> framed;
    framed.reserve(data.size() + 2);
    put_be16(framed, stream_id);
    framed.insert(framed.end(), data.begin(), data.end());
    return sendSessionData(kExternalAccessorySessionId, framed.data(), framed.size());
}

bool LinkLayer::poll(unsigned timeout_ms)
{
    if (state_ == State::kDead)
    {
        return false;
    }

    const auto now = Clock::now();
    unsigned wait = timeout_ms;
    const auto clamp = [&](const std::optional<Clock::time_point>& deadline)
    {
        if (!deadline)
        {
            return;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now).count();
        wait = std::min<unsigned>(wait, remaining <= 0 ? 0u : static_cast<unsigned>(remaining));
    };
    clamp(marker_deadline_);
    clamp(negotiate_deadline_);
    clamp(send_ack_deadline_);
    clamp(recv_ack_deadline_);

    std::vector<uint8_t> chunk = transport_.recv(kRecvChunkSize, wait);
    if (!chunk.empty())
    {
        rx_.insert(rx_.end(), chunk.begin(), chunk.end());
        processRxBuffer();
    }

    fireTimers();
    drainControlQueue();
    return state_ != State::kDead;
}

bool LinkLayer::waitNegotiated(unsigned timeout_ms)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true)
    {
        if (state_ == State::kNormal)
        {
            return true;
        }
        if (state_ == State::kDead)
        {
            SPDLOG_ERROR("[iap2] {}: link died before negotiation completed", config_.tag);
            return false;
        }

        const auto now = Clock::now();
        const unsigned remaining =
            (now >= deadline)
                ? 0u
                : static_cast<unsigned>(
                      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        poll(remaining);
        if (state_ == State::kNormal)
        {
            return true;
        }
        if (Clock::now() >= deadline)
        {
            SPDLOG_ERROR("[iap2] {}: link negotiation timed out after {} ms (state={})", config_.tag,
                         timeout_ms, stateName(state_));
            return false;
        }
    }
}

std::optional<std::vector<uint8_t>> LinkLayer::receiveControlMessage(unsigned timeout_ms)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true)
    {
        if (!control_queue_.empty())
        {
            std::vector<uint8_t> message = std::move(control_queue_.front());
            control_queue_.pop_front();
            return message;
        }
        if (state_ == State::kDead)
        {
            SPDLOG_ERROR("[iap2] {}: link is dead while waiting for a control message", config_.tag);
            return std::nullopt;
        }

        const auto now = Clock::now();
        const unsigned remaining =
            (now >= deadline)
                ? 0u
                : static_cast<unsigned>(
                      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
        poll(remaining);
        if (!control_queue_.empty())
        {
            continue;
        }
        if (Clock::now() >= deadline)
        {
            SPDLOG_WARN("[iap2] {}: no control message within {} ms", config_.tag, timeout_ms);
            return std::nullopt;
        }
    }
}

void LinkLayer::processRxBuffer()
{
    while (state_ != State::kDead)
    {
        const size_t available = rx_.size() - rx_head_;
        if (available < kLinkHeaderSize)
        {
            break;
        }

        const uint8_t* head = rx_.data() + rx_head_;
        if (get_be16(head) != kLinkPacketStart)
        {
            // Resynchronise a byte at a time. The iAP2 detection marker the
            // phone echoes back lands here and is skipped silently.
            ++rx_head_;
            continue;
        }

        const auto header = LinkPacketHeader::parse(head, kLinkHeaderSize);
        if (!header)
        {
            SPDLOG_WARN("[iap2] {}: header checksum failed, resynchronising", config_.tag);
            ++rx_head_;
            continue;
        }
        if (header->length < kLinkHeaderSize)
        {
            SPDLOG_WARN("[iap2] {}: packet declares length {} which is below the {} byte header",
                        config_.tag, header->length, kLinkHeaderSize);
            ++rx_head_;
            continue;
        }
        if (available < header->length)
        {
            // Wait for the rest of the packet.
            break;
        }

        std::optional<std::vector<uint8_t>> payload;
        bool payload_ok = true;
        if (header->length > kLinkHeaderSize)
        {
            const uint8_t* with_checksum = head + kLinkHeaderSize;
            const size_t with_checksum_len = header->length - kLinkHeaderSize;
            if (!checkChecksum(with_checksum, with_checksum_len))
            {
                SPDLOG_WARN("[iap2] {}: payload checksum failed, dropping {} byte packet (seq={} {})",
                            config_.tag, header->length, header->seq, controlName(header->control));
                payload_ok = false;
            }
            else
            {
                payload.emplace(with_checksum, with_checksum + with_checksum_len - 1);
            }
        }

        rx_head_ += header->length;
        if (payload_ok)
        {
            SPDLOG_DEBUG("[iap2] {} < {} seq={} ack={} session={} len={}", config_.tag,
                         controlName(header->control), header->seq, header->ack, header->session_id,
                         header->length);
            handlePacket(*header, payload);
        }
    }

    if (rx_head_ > 0)
    {
        rx_.erase(rx_.begin(), rx_.begin() + static_cast<long>(rx_head_));
        rx_head_ = 0;
    }
}

void LinkLayer::handlePacket(const LinkPacketHeader& header, const std::optional<std::vector<uint8_t>>& payload)
{
    if ((header.control & kControlRst) != 0)
    {
        bailout("device sent a reset (RST) packet");
        return;
    }

    if ((header.control & kControlSyn) != 0)
    {
        if (!payload)
        {
            SPDLOG_WARN("[iap2] {}: SYN packet without a link synchronization payload", config_.tag);
            return;
        }
        const auto lsp = LinkSynchronizationPayload::parse(payload->data(), payload->size());
        if (!lsp)
        {
            return;
        }
        handleSyn(*lsp, header.seq);
    }

    if ((header.control & kControlAck) != 0)
    {
        ++cumulative_received_;
        handleAck(header.ack);
    }

    if ((header.control & kControlEak) != 0 && payload && !payload->empty())
    {
        handleEak(*payload);
    }

    if ((header.control & static_cast<uint8_t>(~kControlAck)) == 0 && payload)
    {
        handleData(*payload, header.seq, header.session_id);
    }

    if (lsp_.max_ack > 0 && cumulative_received_ >= lsp_.max_ack)
    {
        cumulative_received_ = 0;
        last_acked_psn_ = last_received_in_sequence_psn_;
        sendAck();
    }
}

void LinkLayer::handleSyn(const LinkSynchronizationPayload& lsp, uint8_t psn)
{
    if (state_ != State::kNegotiate)
    {
        SPDLOG_WARN("[iap2] {}: ignoring SYN received in state {}", config_.tag, stateName(state_));
        return;
    }

    SPDLOG_DEBUG("[iap2] {}: device link parameters max_outgoing={} max_len={} rto={}ms ack_timeout={}ms "
                 "max_retransmissions={} max_ack={} sessions={}",
                 config_.tag, lsp.max_outgoing, lsp.max_len, lsp.retransmission_timeout, lsp.ack_timeout,
                 lsp.max_retransmissions, lsp.max_ack, lsp.sessions.size());
    for (const LinkSession& session : lsp.sessions)
    {
        SPDLOG_DEBUG("[iap2] {}:   session id={} type={} version={}", config_.tag, session.id, session.type,
                     session.version);
    }

    // The device is authoritative: adopt its parameters wholesale, as LIVI does.
    lsp_ = lsp;
    last_received_in_sequence_psn_ = psn;
    last_acked_psn_ = psn;
    sendAck();
}

void LinkLayer::handleAck(uint8_t ack)
{
    if (state_ == State::kNegotiate)
    {
        state_ = State::kNormal;
        negotiate_deadline_.reset();
        SPDLOG_INFO("[iap2] {}: link negotiated (state -> normal), max_outgoing={} max_len={} max_ack={}",
                    config_.tag, lsp_.max_outgoing, lsp_.max_len, lsp_.max_ack);
    }
    last_sent_acknowledged_psn_ = ack;

    while (!unacked_.empty())
    {
        const uint8_t distance = sequenceDistance(unacked_.front().psn, last_sent_acknowledged_psn_);
        if (distance > 0 && distance <= lsp_.max_ack + 10)
        {
            rearmRecvAckTimer(unacked_.front().timeout);
            break;
        }
        unacked_.erase(unacked_.begin());
    }
    if (unacked_.empty())
    {
        disarmRecvAckTimer();
    }

    size_t guard = queued_.size();
    while (guard-- > 0 && !queued_.empty() &&
           sequenceDistance(sent_psn_, last_sent_acknowledged_psn_) < lsp_.max_outgoing)
    {
        OutPacket packet = std::move(queued_.front());
        queued_.pop_front();
        enqueuePacket(std::move(packet));
    }
}

void LinkLayer::handleEak(const std::vector<uint8_t>& psns)
{
    if (state_ != State::kNormal)
    {
        SPDLOG_WARN("[iap2] {}: ignoring EAK received in state {}", config_.tag, stateName(state_));
        return;
    }

    for (OutPacket& packet : unacked_)
    {
        if (std::find(psns.begin(), psns.end(), packet.psn) == psns.end())
        {
            continue;
        }

        ++packet.counter;
        if (packet.counter == lsp_.max_retransmissions)
        {
            SPDLOG_ERROR("[iap2] {}: packet seq={} hit the retransmission limit ({})", config_.tag,
                         packet.psn, lsp_.max_retransmissions);
            bailout("retransmission limit reached after EAK");
            return;
        }

        SPDLOG_WARN("[iap2] {}: retransmitting seq={} after EAK (attempt {})", config_.tag, packet.psn,
                    packet.counter);
        sendData(packet);
        disarmSendAckTimer();
        rearmRecvAckTimer(packet.timeout);
    }
}

void LinkLayer::handleData(const std::vector<uint8_t>& payload, uint8_t psn, uint8_t session_id)
{
    const int distance = sequenceDistance(psn, last_received_in_sequence_psn_);
    if (distance > static_cast<int>(lsp_.max_outgoing) + 10 || distance == 0)
    {
        SPDLOG_WARN("[iap2] {}: dropping out of window packet seq={} (last in sequence {}, distance {})",
                    config_.tag, psn, last_received_in_sequence_psn_, distance);
        sendAck();
        return;
    }

    received_out_of_sequence_.push_back(InPacket{payload, session_id, psn});

    if (distance > 1)
    {
        SPDLOG_WARN("[iap2] {}: packet seq={} arrived out of sequence (expected {}, distance {})",
                    config_.tag, psn, static_cast<uint8_t>(last_received_in_sequence_psn_ + 1), distance);
        if (distance >= static_cast<int>(lsp_.max_outgoing))
        {
            std::vector<uint8_t> missing;
            uint8_t x = last_received_in_sequence_psn_;
            while (sequenceDistance(psn, x) > 1)
            {
                x = static_cast<uint8_t>(x + 1);
                missing.push_back(x);
            }
            disarmSendAckTimer();
            sendEak(missing);
        }
        return;
    }

    while (true)
    {
        auto it = std::find_if(received_out_of_sequence_.begin(), received_out_of_sequence_.end(),
                               [this](const InPacket& packet)
                               { return sequenceDistance(packet.psn, last_received_in_sequence_psn_) == 1; });
        if (it == received_out_of_sequence_.end())
        {
            break;
        }
        const InPacket packet = std::move(*it);
        received_out_of_sequence_.erase(it);
        last_received_in_sequence_psn_ = packet.psn;
        deliver(packet.data, packet.session_id);
    }

    if (lsp_.max_ack == 0)
    {
        // zero_ack: the peer does not want cumulative acknowledgements.
    }
    else if (sequenceDistance(last_received_in_sequence_psn_, last_acked_psn_) >=
             lsp_.max_outgoing - config_.max_outgoing_delta)
    {
        disarmSendAckTimer();
        last_acked_psn_ = last_received_in_sequence_psn_;
        sendAck();
    }
    else
    {
        rearmSendAckTimer();
    }
}

void LinkLayer::deliver(const std::vector<uint8_t>& payload, uint8_t session_id)
{
    if (session_id == kControlSessionId)
    {
        control_rx_.insert(control_rx_.end(), payload.begin(), payload.end());
        parseControlSession();
    }
    else if (session_id == kExternalAccessorySessionId)
    {
        if (payload.size() < 2)
        {
            SPDLOG_WARN("[iap2] {}: external accessory payload is only {} bytes, need a 2 byte stream id",
                        config_.tag, payload.size());
            return;
        }
        const uint16_t stream_id = get_be16(payload.data());
        if (external_accessory_handler_)
        {
            external_accessory_handler_(stream_id, std::vector<uint8_t>(payload.begin() + 2, payload.end()));
        }
        else
        {
            SPDLOG_DEBUG("[iap2] {}: dropping {} bytes for external accessory stream {} (no handler)",
                         config_.tag, payload.size() - 2, stream_id);
        }
    }
    else if (session_id == kFileTransferSessionId)
    {
        if (file_transfer_handler_)
        {
            file_transfer_handler_(payload);
        }
        else
        {
            SPDLOG_DEBUG("[iap2] {}: dropping {} file transfer bytes (no handler)", config_.tag,
                         payload.size());
        }
    }
    else
    {
        SPDLOG_WARN("[iap2] {}: {} bytes arrived on unknown session {}", config_.tag, payload.size(),
                    session_id);
    }
}

void LinkLayer::parseControlSession()
{
    size_t head = 0;
    while (true)
    {
        const size_t available = control_rx_.size() - head;
        const auto total = csm::peekLength(control_rx_.data() + head, available);
        if (!total)
        {
            // Fewer than 6 bytes; wait for the rest of the header.
            break;
        }
        if (*total == 0)
        {
            SPDLOG_ERROR("[iap2] {}: control session lost framing, resynchronising", config_.tag);
            ++head;
            continue;
        }
        if (available < *total)
        {
            SPDLOG_DEBUG("[iap2] {}: control message needs {} bytes, {} buffered so far", config_.tag, *total,
                         available);
            break;
        }

        std::vector<uint8_t> message(control_rx_.begin() + static_cast<long>(head),
                                     control_rx_.begin() + static_cast<long>(head + *total));
        const uint16_t msg_id = get_be16(message.data() + 4);
        SPDLOG_DEBUG("[iap2] {}: control message 0x{:04X} ({}), {} bytes", config_.tag, msg_id,
                     messageIdName(msg_id), message.size());
        control_queue_.push_back(std::move(message));
        head += *total;
    }

    if (head > 0)
    {
        control_rx_.erase(control_rx_.begin(), control_rx_.begin() + static_cast<long>(head));
    }
    if (control_rx_.size() > kMaxControlBuffer)
    {
        SPDLOG_ERROR("[iap2] {}: control session buffer grew to {} bytes without a complete message",
                     config_.tag, control_rx_.size());
        control_rx_.clear();
    }
}

void LinkLayer::drainControlQueue()
{
    if (!control_handler_)
    {
        return;
    }
    while (!control_queue_.empty())
    {
        std::vector<uint8_t> message = std::move(control_queue_.front());
        control_queue_.pop_front();
        control_handler_(message);
    }
}

void LinkLayer::fireTimers()
{
    const auto now = Clock::now();

    if (marker_deadline_ && now >= *marker_deadline_)
    {
        marker_deadline_.reset();
        sendDetectMarker();
    }
    if (negotiate_deadline_ && now >= *negotiate_deadline_)
    {
        negotiate_deadline_.reset();
        SPDLOG_DEBUG("[iap2] {}: no answer to SYN, resending", config_.tag);
        sendNegotiate();
    }
    if (send_ack_deadline_ && now >= *send_ack_deadline_)
    {
        send_ack_deadline_.reset();
        onSendAckTimer();
    }
    if (recv_ack_deadline_ && now >= *recv_ack_deadline_)
    {
        recv_ack_deadline_.reset();
        onExpectAckTimer();
    }
}

void LinkLayer::onSendAckTimer()
{
    if (state_ != State::kNormal)
    {
        return;
    }
    last_acked_psn_ = last_received_in_sequence_psn_;
    sendAck();
}

void LinkLayer::onExpectAckTimer()
{
    if (unacked_.empty() || state_ != State::kNormal)
    {
        return;
    }

    std::sort(unacked_.begin(), unacked_.end(),
              [](const OutPacket& a, const OutPacket& b) { return a.timeout < b.timeout; });

    OutPacket& packet = unacked_.front();
    packet.timeout = Clock::now() + std::chrono::milliseconds(lsp_.retransmission_timeout);
    ++packet.counter;
    if (packet.counter == lsp_.max_retransmissions)
    {
        SPDLOG_ERROR("[iap2] {}: packet seq={} was never acknowledged after {} attempts", config_.tag,
                     packet.psn, packet.counter);
        bailout("retransmission limit reached");
        return;
    }

    SPDLOG_WARN("[iap2] {}: retransmitting seq={} (attempt {})", config_.tag, packet.psn, packet.counter);
    sendData(packet);
    rearmRecvAckTimer(unacked_.size() == 1 ? unacked_.front().timeout : unacked_[1].timeout);
}

void LinkLayer::rearmSendAckTimer()
{
    send_ack_deadline_ = Clock::now() + std::chrono::milliseconds(lsp_.ack_timeout);
}

void LinkLayer::rearmRecvAckTimer(Clock::time_point deadline) { recv_ack_deadline_ = deadline; }

}  // namespace iap2
