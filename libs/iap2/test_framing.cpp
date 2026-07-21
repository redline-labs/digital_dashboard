// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/link_layer.py
#include "iap2/link_layer.h"
#include "iap2/messages.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

namespace
{

namespace csm = iap2::csm;

int failures = 0;

void expect(bool condition, const char* what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

// ---------------------------------------------------------------------------
// A transport that just buffers bytes in both directions.
// ---------------------------------------------------------------------------
class FakeTransport : public iap2::Iap2Transport
{
  public:
    bool send(const uint8_t* data, size_t len) override
    {
        if (fail_sends)
        {
            return false;
        }
        sent.insert(sent.end(), data, data + len);
        return true;
    }

    std::vector<uint8_t> recv(size_t max_len, unsigned /*timeout_ms*/) override
    {
        const size_t take = std::min(max_len, inbox.size());
        std::vector<uint8_t> out(inbox.begin(), inbox.begin() + static_cast<long>(take));
        inbox.erase(inbox.begin(), inbox.begin() + static_cast<long>(take));
        return out;
    }

    void push(const std::vector<uint8_t>& data) { inbox.insert(inbox.end(), data.begin(), data.end()); }

    std::vector<uint8_t> sent;
    std::vector<uint8_t> inbox;
    bool fail_sends = false;
};

struct CapturedPacket
{
    iap2::LinkPacketHeader header;
    std::vector<uint8_t> payload;
};

// Pulls every complete link packet out of the transport's sent buffer. Bytes
// that are not part of a packet (e.g. the iAP2 detection marker) are skipped.
std::vector<CapturedPacket> drainSent(FakeTransport& transport)
{
    std::vector<CapturedPacket> packets;
    size_t offset = 0;
    while (offset + iap2::kLinkHeaderSize <= transport.sent.size())
    {
        const auto header = iap2::LinkPacketHeader::parse(transport.sent.data() + offset,
                                                          transport.sent.size() - offset);
        if (!header)
        {
            ++offset;
            continue;
        }
        if (offset + header->length > transport.sent.size())
        {
            break;
        }

        CapturedPacket packet;
        packet.header = *header;
        if (header->length > iap2::kLinkHeaderSize)
        {
            packet.payload.assign(
                transport.sent.begin() + static_cast<long>(offset + iap2::kLinkHeaderSize),
                transport.sent.begin() + static_cast<long>(offset + header->length - 1));
        }
        packets.push_back(std::move(packet));
        offset += header->length;
    }
    transport.sent.clear();
    return packets;
}

// Builds a link packet the way a phone would.
std::vector<uint8_t> buildPacket(uint8_t control, uint8_t seq, uint8_t ack, uint8_t session_id,
                                 const std::vector<uint8_t>& payload)
{
    iap2::LinkPacketHeader header;
    header.control = control;
    header.seq = seq;
    header.ack = ack;
    header.session_id = session_id;
    header.length = payload.empty() ? static_cast<uint16_t>(iap2::kLinkHeaderSize)
                                    : static_cast<uint16_t>(payload.size() + iap2::kLinkHeaderSize + 1);

    std::vector<uint8_t> frame = header.pack();
    if (!payload.empty())
    {
        frame.insert(frame.end(), payload.begin(), payload.end());
        frame.push_back(iap2::genChecksum(payload.data(), payload.size()));
    }
    return frame;
}

iap2::LinkSynchronizationPayload deviceLsp(uint8_t max_outgoing, uint16_t max_len, uint16_t rto,
                                           uint16_t ack_timeout, uint8_t max_retransmissions,
                                           uint8_t max_ack)
{
    iap2::LinkSynchronizationPayload lsp;
    lsp.max_outgoing = max_outgoing;
    lsp.max_len = max_len;
    lsp.retransmission_timeout = rto;
    lsp.ack_timeout = ack_timeout;
    lsp.max_retransmissions = max_retransmissions;
    lsp.max_ack = max_ack;
    lsp.sessions = {
        iap2::LinkSession{iap2::kControlSessionId, 0, 2},
        iap2::LinkSession{iap2::kExternalAccessorySessionId, 2, 1},
        iap2::LinkSession{iap2::kFileTransferSessionId, 1, 2},
    };
    return lsp;
}

// Brings a link all the way to the normal state and returns the psn the device
// used for its SYN.
uint8_t negotiate(iap2::LinkLayer& link, FakeTransport& transport,
                  const iap2::LinkSynchronizationPayload& lsp, uint8_t device_seq = 50)
{
    link.start();
    drainSent(transport);
    transport.push(buildPacket(iap2::kControlSyn | iap2::kControlAck, device_seq, link.sentPsn(), 0,
                               lsp.pack()));
    link.poll(0);
    return device_seq;
}

std::vector<uint8_t> asBytes(const char* text)
{
    std::vector<uint8_t> out;
    for (const char* p = text; *p != 0; ++p)
    {
        out.push_back(static_cast<uint8_t>(*p));
    }
    return out;
}

// A signer that never touches hardware.
class StubSigner : public iap2::MfiSigner
{
  public:
    explicit StubSigner(int major) : major_(major) {}

    std::optional<std::vector<uint8_t>> certificate() override { return std::vector<uint8_t>(300, 0xC7); }

    std::optional<std::vector<uint8_t>> signChallenge(const std::vector<uint8_t>& challenge) override
    {
        last_challenge = challenge;
        return std::vector<uint8_t>(128, 0x5A);
    }

    int protocolMajor() override { return major_; }

    std::vector<uint8_t> last_challenge;

  private:
    int major_;
};

// ---------------------------------------------------------------------------
// Checksums and packet framing.
// ---------------------------------------------------------------------------
void testChecksums()
{
    // A bare ACK header: FF 5A 00 09 40 00 00 00, checksum 0x5E.
    const std::vector<uint8_t> header_bytes = {0xFF, 0x5A, 0x00, 0x09, 0x40, 0x00, 0x00, 0x00};
    expect(iap2::genChecksum(header_bytes.data(), header_bytes.size()) == 0x5E,
           "known header checksum is 0x5E");

    std::vector<uint8_t> full = header_bytes;
    full.push_back(0x5E);
    expect(iap2::checkChecksum(full.data(), full.size()), "checksummed header verifies");

    full.back() = 0x5F;
    expect(!iap2::checkChecksum(full.data(), full.size()), "corrupted checksum is rejected");

    const std::vector<uint8_t> empty;
    expect(iap2::genChecksum(empty.data(), 0) == 0x00, "checksum of nothing is zero");
}

void testHeaderRoundTrip()
{
    iap2::LinkPacketHeader header;
    header.length = 1234;
    header.control = iap2::kControlAck;
    header.seq = 0x7B;
    header.ack = 0x2A;
    header.session_id = iap2::kControlSessionId;

    std::vector<uint8_t> packed = header.pack();
    expect(packed.size() == iap2::kLinkHeaderSize, "header packs to 9 bytes");
    expect(packed[0] == 0xFF && packed[1] == 0x5A, "header starts with 0xFF5A");

    const auto parsed = iap2::LinkPacketHeader::parse(packed.data(), packed.size());
    expect(parsed.has_value(), "header parses back");
    if (parsed)
    {
        expect(parsed->length == header.length && parsed->control == header.control &&
                   parsed->seq == header.seq && parsed->ack == header.ack &&
                   parsed->session_id == header.session_id,
               "header round trip preserves every field");
    }

    // A corrupted header checksum must be rejected.
    std::vector<uint8_t> corrupt = packed;
    corrupt[8] = static_cast<uint8_t>(corrupt[8] ^ 0xFF);
    expect(!iap2::LinkPacketHeader::parse(corrupt.data(), corrupt.size()),
           "header with a bad checksum is rejected");

    // A corrupted start marker must be rejected too.
    corrupt = packed;
    corrupt[1] = 0x5B;
    expect(!iap2::LinkPacketHeader::parse(corrupt.data(), corrupt.size()),
           "header with a bad start marker is rejected");

    expect(!iap2::LinkPacketHeader::parse(packed.data(), 8), "short header is rejected");
}

void testLspRoundTrip()
{
    const iap2::LinkSynchronizationPayload lsp = deviceLsp(4, 1024, 4000, 500, 4, 3);
    const std::vector<uint8_t> packed = lsp.pack();
    expect(packed.size() == iap2::LinkSynchronizationPayload::kFixedSize + 9,
           "link synchronization payload packs to 10 + 3*sessions bytes");

    const auto parsed = iap2::LinkSynchronizationPayload::parse(packed.data(), packed.size());
    expect(parsed.has_value(), "link synchronization payload parses back");
    if (parsed)
    {
        expect(parsed->max_outgoing == 4 && parsed->max_len == 1024 && parsed->retransmission_timeout == 4000 &&
                   parsed->ack_timeout == 500 && parsed->max_retransmissions == 4 && parsed->max_ack == 3,
               "link synchronization payload round trip preserves the parameters");
        expect(parsed->sessions == lsp.sessions, "link synchronization payload round trip keeps sessions");
    }

    std::vector<uint8_t> bad_version = packed;
    bad_version[0] = 0x02;
    expect(!iap2::LinkSynchronizationPayload::parse(bad_version.data(), bad_version.size()),
           "unsupported link synchronization payload version is rejected");
    expect(!iap2::LinkSynchronizationPayload::parse(packed.data(), 5),
           "short link synchronization payload is rejected");
}

// ---------------------------------------------------------------------------
// Negotiation.
// ---------------------------------------------------------------------------
void testStartAndNegotiation()
{
    FakeTransport transport;
    iap2::LinkLayer link(transport, iap2::LinkConfig{});
    expect(link.start(), "start() succeeds");

    // The detection marker goes out first, ahead of any link packet.
    expect(transport.sent.size() > iap2::kIap2MarkerSize, "start() wrote marker and SYN");
    expect(std::equal(std::begin(iap2::kIap2Marker), std::end(iap2::kIap2Marker), transport.sent.begin()),
           "detection marker is sent first");

    const auto packets = drainSent(transport);
    expect(packets.size() == 1, "start() sends exactly one SYN packet");
    if (packets.empty())
    {
        return;
    }

    expect((packets[0].header.control & iap2::kControlSyn) != 0, "first packet is a SYN");
    expect(packets[0].header.seq == 99, "SYN uses the initial sequence number 99");
    expect(packets[0].header.session_id == 0, "SYN is not bound to a session");

    const auto lsp = iap2::LinkSynchronizationPayload::parse(packets[0].payload.data(),
                                                             packets[0].payload.size());
    expect(lsp.has_value(), "SYN carries a link synchronization payload");
    if (lsp)
    {
        expect(lsp->max_outgoing == 4, "accessory advertises max_outgoing 4");
        expect(lsp->retransmission_timeout == 0 && lsp->ack_timeout == 0 && lsp->max_retransmissions == 0 &&
                   lsp->max_ack == 0,
               "zero_ack zeroes every retransmission parameter");
        expect(lsp->sessions.size() == 3, "accessory advertises three sessions");
        if (lsp->sessions.size() == 3)
        {
            expect(lsp->sessions[0] == (iap2::LinkSession{iap2::kControlSessionId, 0, 2}),
                   "control session is id 10, type 0, version 2");
            expect(lsp->sessions[1] == (iap2::LinkSession{iap2::kExternalAccessorySessionId, 2, 1}),
                   "external accessory session is id 11, type 2, version 1");
            expect(lsp->sessions[2] == (iap2::LinkSession{iap2::kFileTransferSessionId, 1, 2}),
                   "file transfer session is id 12, type 1, version 2");
        }
    }

    // The phone answers with SYN|ACK.
    transport.push(buildPacket(iap2::kControlSyn | iap2::kControlAck, 50, 99, 0,
                               deviceLsp(4, 65535, 0, 0, 0, 0).pack()));
    link.poll(0);

    expect(link.negotiated(), "SYN|ACK moves the link to the normal state");
    expect(link.lastReceivedPsn() == 50, "the device SYN seeds the inbound sequence number");

    const auto acks = drainSent(transport);
    expect(acks.size() == 1, "the accessory answers the SYN with a single ACK");
    if (!acks.empty())
    {
        expect((acks[0].header.control & iap2::kControlAck) != 0, "the answer is an ACK");
        expect(acks[0].header.ack == 50, "the ACK acknowledges the device sequence number");
        expect(acks[0].header.seq == 99, "the ACK carries our own sequence number");
        expect(acks[0].header.length == iap2::kLinkHeaderSize, "a bare ACK is header only");
    }

    // A reset from the phone kills the link.
    transport.push(buildPacket(iap2::kControlRst, 51, 100, 0, {}));
    link.poll(0);
    expect(!link.alive(), "RST kills the link");
}

// ---------------------------------------------------------------------------
// Control session data path.
// ---------------------------------------------------------------------------
void testControlSessionDataPath()
{
    FakeTransport transport;
    iap2::LinkLayer link(transport, iap2::LinkConfig{});
    uint8_t device_seq = negotiate(link, transport, deviceLsp(4, 65535, 0, 0, 0, 0));
    drainSent(transport);

    // Outbound.
    const std::vector<uint8_t> subscribe = iap2::encodeStartPowerUpdates();
    expect(link.sendControlMessage(subscribe), "sendControlMessage succeeds after negotiation");

    auto packets = drainSent(transport);
    expect(packets.size() == 1, "a small control message becomes one packet");
    if (!packets.empty())
    {
        expect(packets[0].header.session_id == iap2::kControlSessionId, "data goes out on session 10");
        expect(packets[0].header.control == iap2::kControlAck, "data packets carry the ACK bit");
        expect(packets[0].header.seq == 100, "the first data packet uses sequence number 100");
        expect(packets[0].header.ack == device_seq, "the data packet acknowledges the device");
        expect(packets[0].payload == subscribe, "the payload is the encoded control message");
        expect(packets[0].header.length == subscribe.size() + iap2::kLinkHeaderSize + 1,
               "packet length covers header, payload and payload checksum");
    }

    // Inbound.
    csm::ParamList media;
    iap2::csm::addString(media, 1, "Bohemian Rhapsody");
    iap2::csm::addString(media, 12, "Queen");
    iap2::csm::addU32(media, 4, 355000);
    csm::ParamList playback;
    iap2::csm::addU8(playback, 0, static_cast<uint8_t>(iap2::PlaybackStatus::kPlaying));
    iap2::csm::addU32(playback, 1, 42000);
    iap2::csm::addString(playback, 7, "Music");
    csm::ParamList params;
    iap2::csm::addGroup(params, 0, media);
    iap2::csm::addGroup(params, 1, playback);
    const std::vector<uint8_t> now_playing = iap2::csm::encodeMessage(iap2::kMsgNowPlayingUpdate, params);

    device_seq = static_cast<uint8_t>(device_seq + 1);
    transport.push(buildPacket(iap2::kControlAck, device_seq, 100, iap2::kControlSessionId, now_playing));
    link.poll(0);

    const auto received = link.receiveControlMessage(0);
    expect(received.has_value(), "an inbound control message is delivered");
    expect(received && *received == now_playing, "the inbound control message round trips byte for byte");

    if (received)
    {
        const auto message = iap2::csm::parseMessage(*received);
        expect(message && message->id == iap2::kMsgNowPlayingUpdate, "the inbound message is a NowPlayingUpdate");
        if (message)
        {
            const auto decoded = iap2::decodeNowPlayingUpdate(message->params);
            expect(decoded && decoded->title == "Bohemian Rhapsody", "title decodes");
            expect(decoded && decoded->artist == "Queen", "artist decodes");
            expect(decoded && decoded->duration_ms == 355000u, "duration decodes");
            expect(decoded && decoded->status == iap2::PlaybackStatus::kPlaying, "playback status decodes");
            expect(decoded && decoded->elapsed_ms == 42000u, "elapsed time decodes");
            expect(decoded && decoded->app_name == "Music", "app name decodes");
        }
    }

    // A corrupted payload checksum must drop the whole packet without killing
    // the link or advancing the inbound sequence number.
    const uint8_t corrupt_seq = static_cast<uint8_t>(device_seq + 1);
    std::vector<uint8_t> corrupt =
        buildPacket(iap2::kControlAck, corrupt_seq, 100, iap2::kControlSessionId, now_playing);
    corrupt.back() = static_cast<uint8_t>(corrupt.back() ^ 0xFF);
    transport.push(corrupt);
    link.poll(0);

    expect(!link.receiveControlMessage(0).has_value(),
           "a packet with a bad payload checksum delivers nothing");
    expect(link.alive(), "a bad payload checksum does not kill the link");
    expect(link.lastReceivedPsn() == device_seq, "a dropped packet does not advance the sequence number");

    // The retransmission arrives intact.
    transport.push(buildPacket(iap2::kControlAck, corrupt_seq, 100, iap2::kControlSessionId, now_playing));
    link.poll(0);
    expect(link.receiveControlMessage(0).has_value(), "the retransmitted packet is accepted");
}

// ---------------------------------------------------------------------------
// Fragmentation and reassembly.
// ---------------------------------------------------------------------------
void testReassembly()
{
    FakeTransport transport;
    iap2::LinkLayer link(transport, iap2::LinkConfig{});
    uint8_t device_seq = negotiate(link, transport, deviceLsp(4, 65535, 0, 0, 0, 0));
    drainSent(transport);

    // A single control message split across three link packets.
    const std::vector<uint8_t> big = iap2::encodeIdentificationInformation(iap2::IdentificationConfig{});
    expect(big.size() > 90, "identification information is big enough to fragment");

    const size_t third = big.size() / 3;
    const std::vector<std::vector<uint8_t>> chunks = {
        {big.begin(), big.begin() + static_cast<long>(third)},
        {big.begin() + static_cast<long>(third), big.begin() + static_cast<long>(2 * third)},
        {big.begin() + static_cast<long>(2 * third), big.end()},
    };
    for (const auto& chunk : chunks)
    {
        device_seq = static_cast<uint8_t>(device_seq + 1);
        transport.push(buildPacket(iap2::kControlAck, device_seq, 100, iap2::kControlSessionId, chunk));
        link.poll(0);
    }

    const auto reassembled = link.receiveControlMessage(0);
    expect(reassembled.has_value(), "a fragmented control message is reassembled");
    expect(reassembled && *reassembled == big, "the reassembled message matches the original");
    expect(!link.receiveControlMessage(0).has_value(), "no extra message falls out of reassembly");

    // Two whole messages inside one link packet must both come out.
    std::vector<uint8_t> pair = iap2::encodeStartPowerUpdates();
    const std::vector<uint8_t> second = iap2::encodeStartCallStateUpdates();
    pair.insert(pair.end(), second.begin(), second.end());
    device_seq = static_cast<uint8_t>(device_seq + 1);
    transport.push(buildPacket(iap2::kControlAck, device_seq, 100, iap2::kControlSessionId, pair));
    link.poll(0);

    const auto first_of_pair = link.receiveControlMessage(0);
    const auto second_of_pair = link.receiveControlMessage(0);
    expect(first_of_pair && *first_of_pair == iap2::encodeStartPowerUpdates(),
           "the first of two coalesced messages is delivered");
    expect(second_of_pair && *second_of_pair == second, "the second of two coalesced messages is delivered");
}

void testFragmentedSend()
{
    FakeTransport transport;
    iap2::LinkLayer link(transport, iap2::LinkConfig{});
    // A phone that only accepts 64 byte packets: 54 payload bytes each.
    negotiate(link, transport, deviceLsp(16, 64, 0, 0, 0, 0));
    drainSent(transport);

    expect(link.maxPayload() == 64 - iap2::kLinkHeaderSize - 1, "max payload accounts for the framing");

    const std::vector<uint8_t> big = iap2::encodeIdentificationInformation(iap2::IdentificationConfig{});
    expect(link.sendControlMessage(big), "a large control message is accepted");

    const auto packets = drainSent(transport);
    const size_t expected = (big.size() + link.maxPayload() - 1) / link.maxPayload();
    expect(packets.size() == expected, "a large control message is split into the expected packet count");

    std::vector<uint8_t> rebuilt;
    uint8_t expected_psn = 100;
    bool sequential = true;
    for (const auto& packet : packets)
    {
        sequential = sequential && packet.header.seq == expected_psn;
        sequential = sequential && packet.header.session_id == iap2::kControlSessionId;
        sequential = sequential && packet.payload.size() <= link.maxPayload();
        sequential = sequential && packet.header.length <= 64;
        expected_psn = static_cast<uint8_t>(expected_psn + 1);
        rebuilt.insert(rebuilt.end(), packet.payload.begin(), packet.payload.end());
    }
    expect(sequential, "fragments use consecutive sequence numbers and respect the packet limit");
    expect(rebuilt == big, "the fragments concatenate back into the original message");
}

// ---------------------------------------------------------------------------
// Out of sequence handling, EAK, retransmission.
// ---------------------------------------------------------------------------
void testOutOfSequence()
{
    FakeTransport transport;
    iap2::LinkLayer link(transport, iap2::LinkConfig{});
    // A phone that wants retransmissions: max_outgoing 4, max_ack 3.
    const uint8_t device_seq = negotiate(link, transport, deviceLsp(4, 65535, 4000, 500, 4, 3));
    drainSent(transport);

    const std::vector<uint8_t> first = iap2::encodeStartPowerUpdates();
    const std::vector<uint8_t> second = iap2::encodeStartCallStateUpdates();

    // Deliver seq+2 before seq+1: it must be held back.
    transport.push(buildPacket(iap2::kControlAck, static_cast<uint8_t>(device_seq + 2), 99,
                               iap2::kControlSessionId, second));
    link.poll(0);
    expect(!link.receiveControlMessage(0).has_value(), "an out of sequence packet is held back");
    expect(link.lastReceivedPsn() == device_seq, "an out of sequence packet does not advance the sequence");

    // The gap closes and both come out in order.
    transport.push(buildPacket(iap2::kControlAck, static_cast<uint8_t>(device_seq + 1), 99,
                               iap2::kControlSessionId, first));
    link.poll(0);

    const auto a = link.receiveControlMessage(0);
    const auto b = link.receiveControlMessage(0);
    expect(a && *a == first, "the missing packet is delivered first");
    expect(b && *b == second, "the held back packet is delivered second");
    expect(link.lastReceivedPsn() == static_cast<uint8_t>(device_seq + 2),
           "the inbound sequence number catches up");

    // A gap of max_outgoing must trigger an EAK naming the missing packets.
    drainSent(transport);
    const uint8_t base = link.lastReceivedPsn();
    transport.push(buildPacket(iap2::kControlAck, static_cast<uint8_t>(base + 4), 99,
                               iap2::kControlSessionId, first));
    link.poll(0);

    const auto packets = drainSent(transport);
    const auto eak = std::find_if(packets.begin(), packets.end(), [](const CapturedPacket& packet)
                                  { return (packet.header.control & iap2::kControlEak) != 0; });
    expect(eak != packets.end(), "a large gap triggers an EAK");
    if (eak != packets.end())
    {
        const std::vector<uint8_t> missing = {static_cast<uint8_t>(base + 1), static_cast<uint8_t>(base + 2),
                                              static_cast<uint8_t>(base + 3)};
        expect(eak->payload == missing, "the EAK names exactly the missing sequence numbers");
    }
}

void testEakRetransmission()
{
    FakeTransport transport;
    iap2::LinkLayer link(transport, iap2::LinkConfig{});
    negotiate(link, transport, deviceLsp(4, 65535, 4000, 500, 4, 3));
    drainSent(transport);

    const std::vector<uint8_t> message = iap2::encodeStartPowerUpdates();
    link.sendControlMessage(message);
    const auto sent = drainSent(transport);
    expect(sent.size() == 1, "one data packet goes out");
    if (sent.empty())
    {
        return;
    }
    const uint8_t psn = sent[0].header.seq;

    // The phone reports it never saw that packet.
    transport.push(buildPacket(iap2::kControlEak, 60, 99, 0, {psn}));
    link.poll(0);

    const auto retransmitted = drainSent(transport);
    const auto found = std::find_if(retransmitted.begin(), retransmitted.end(),
                                    [&](const CapturedPacket& packet)
                                    { return packet.header.seq == psn && packet.payload == message; });
    expect(found != retransmitted.end(), "an EAK retransmits the named packet with the same sequence number");
}

void testQueuedBeforeNegotiation()
{
    FakeTransport transport;
    iap2::LinkLayer link(transport, iap2::LinkConfig{});
    link.start();
    drainSent(transport);

    const std::vector<uint8_t> message = iap2::encodeStartPowerUpdates();
    link.sendControlMessage(message);
    expect(drainSent(transport).empty(), "control messages are queued until the link is negotiated");

    transport.push(buildPacket(iap2::kControlSyn | iap2::kControlAck, 50, 99, 0,
                               deviceLsp(4, 65535, 0, 0, 0, 0).pack()));
    link.poll(0);

    const auto packets = drainSent(transport);
    const auto found = std::find_if(packets.begin(), packets.end(), [&](const CapturedPacket& packet)
                                    { return packet.payload == message; });
    expect(found != packets.end(), "queued control messages flush once the link is negotiated");
}

// ---------------------------------------------------------------------------
// Control session messages.
// ---------------------------------------------------------------------------
void testCsmFraming()
{
    // AuthenticationResponse with a two byte response, hand checked against the
    // Python struct layout: 4040 000C AA03 | 0006 0000 DEAD.
    const std::vector<uint8_t> expected = {0x40, 0x40, 0x00, 0x0C, 0xAA, 0x03,
                                           0x00, 0x06, 0x00, 0x00, 0xDE, 0xAD};
    const auto encoded = iap2::encodeAuthenticationResponse({0xDE, 0xAD});
    expect(encoded == expected, "AuthenticationResponse matches the byte-exact wire format");

    const auto message = iap2::csm::parseMessage(encoded);
    expect(message && message->id == iap2::kMsgAuthenticationResponse, "AuthenticationResponse parses back");
    expect(message && message->params.size() == 1, "AuthenticationResponse has one parameter");
    expect(message && iap2::csm::getBytes(message->params, 0) == std::vector<uint8_t>({0xDE, 0xAD}),
           "AuthenticationResponse payload round trips");

    // Every scalar type round trips.
    csm::ParamList params;
    iap2::csm::addNone(params, 0);
    iap2::csm::addBool(params, 1, true);
    iap2::csm::addU8(params, 2, 0xAB);
    iap2::csm::addU16(params, 3, 0xBEEF);
    iap2::csm::addI16(params, 4, -1234);
    iap2::csm::addU32(params, 5, 0xDEADBEEF);
    iap2::csm::addU64(params, 6, 0x0123456789ABCDEFULL);
    iap2::csm::addString(params, 7, "hello");
    iap2::csm::addBytes(params, 8, {1, 2, 3});
    csm::ParamList nested;
    iap2::csm::addString(nested, 0, "fe80::1");
    iap2::csm::addU16(nested, 1, 7000);
    iap2::csm::addGroup(params, 9, nested);

    const auto blob = iap2::csm::encodeMessage(0x1234, params);
    const auto parsed = iap2::csm::parseMessage(blob);
    expect(parsed && parsed->id == 0x1234, "generic message id round trips");
    if (parsed)
    {
        const auto& p = parsed->params;
        expect(iap2::csm::has(p, 0) && iap2::csm::find(p, 0)->data.empty(), "none-like parameter round trips");
        expect(iap2::csm::getBool(p, 1) == true, "bool round trips");
        expect(iap2::csm::getU8(p, 2) == 0xAB, "uint8 round trips");
        expect(iap2::csm::getU16(p, 3) == 0xBEEF, "uint16 round trips");
        expect(iap2::csm::getI16(p, 4) == -1234, "int16 round trips");
        expect(iap2::csm::getU32(p, 5) == 0xDEADBEEFu, "uint32 round trips");
        expect(iap2::csm::getU64(p, 6) == 0x0123456789ABCDEFULL, "uint64 round trips");
        expect(iap2::csm::getString(p, 7) == "hello", "string round trips without its NUL");
        expect(iap2::csm::getBytes(p, 8) == std::vector<uint8_t>({1, 2, 3}), "bytes round trip");
        const auto group = iap2::csm::getGroup(p, 9);
        expect(group && iap2::csm::getString(*group, 0) == "fe80::1", "nested group string round trips");
        expect(group && iap2::csm::getU16(*group, 1) == 7000, "nested group uint16 round trips");
        expect(!iap2::csm::getU16(p, 99).has_value(), "a missing parameter reads as absent");
    }

    // Malformed frames are rejected rather than throwing.
    std::vector<uint8_t> bad_start = blob;
    bad_start[0] = 0x41;
    expect(!iap2::csm::parseMessage(bad_start), "a bad CSM start marker is rejected");

    std::vector<uint8_t> truncated(blob.begin(), blob.begin() + 4);
    expect(!iap2::csm::parseMessage(truncated), "a truncated CSM is rejected");

    std::vector<uint8_t> lying = blob;
    lying[2] = 0xFF;
    lying[3] = 0xFF;
    expect(!iap2::csm::parseMessage(lying), "a CSM claiming more bytes than it has is rejected");

    expect(iap2::csm::peekLength(blob.data(), 3) == std::nullopt, "peekLength waits for a whole header");
    expect(iap2::csm::peekLength(blob.data(), blob.size()) == blob.size(), "peekLength reports the frame size");
    expect(iap2::csm::peekLength(bad_start.data(), bad_start.size()) == size_t{0},
           "peekLength flags a broken frame");
}

void testIdentification()
{
    iap2::IdentificationConfig config;
    config.name = "Mercedes 190E";
    const auto encoded = iap2::encodeIdentificationInformation(config);
    const auto message = iap2::csm::parseMessage(encoded);
    expect(message && message->id == iap2::kMsgIdentificationInformation, "IdentificationInformation encodes");
    if (!message)
    {
        return;
    }

    const auto& params = message->params;
    expect(iap2::csm::getString(params, 0) == "Mercedes 190E", "accessory name round trips");
    expect(iap2::csm::getU8(params, 8) == static_cast<uint8_t>(iap2::PowerProvidingCapability::kAdvanced),
           "power providing capability is ADVANCED");
    expect(iap2::csm::getU16(params, 9) == 20, "maximum current drawn from device round trips");
    expect(iap2::csm::getString(params, 12) == "en", "current language round trips");

    // The USB host transport component is what makes wired CarPlay work.
    const auto usb_host = iap2::csm::getGroup(params, 16);
    expect(usb_host.has_value(), "identification advertises a USBHostTransportComponent");
    if (usb_host)
    {
        expect(iap2::csm::getU16(*usb_host, 0) == 0, "USB host component id");
        expect(iap2::csm::getString(*usb_host, 1) == "USBHostTransport", "USB host component name");
        expect(iap2::csm::has(*usb_host, 2), "USB host component supports iAP2");
        expect(iap2::csm::getU8(*usb_host, 3) == 3, "car_play_interface_number is 3");
        expect(iap2::csm::has(*usb_host, 4) && iap2::csm::find(*usb_host, 4)->data.empty(),
               "supports_car_play is a none-like flag");
    }

    // The message id lists.
    const auto sent_blob = iap2::csm::getBytes(params, 6);
    const auto received_blob = iap2::csm::getBytes(params, 7);
    expect(sent_blob && sent_blob->size() % 2 == 0, "messages_sent_by_accessory is a list of 16 bit ids");
    expect(received_blob && received_blob->size() % 2 == 0,
           "messages_received_from_accessory is a list of 16 bit ids");

    const auto contains = [](const std::optional<std::vector<uint8_t>>& blob, uint16_t id)
    {
        if (!blob)
        {
            return false;
        }
        for (size_t i = 0; i + 1 < blob->size(); i += 2)
        {
            if (((*blob)[i] << 8 | (*blob)[i + 1]) == id)
            {
                return true;
            }
        }
        return false;
    };
    expect(contains(sent_blob, iap2::kMsgCarPlayStartSession), "we advertise sending CarPlayStartSession");
    expect(contains(sent_blob, iap2::kMsgPowerSourceUpdate), "we advertise sending PowerSourceUpdate");
    expect(contains(sent_blob, iap2::kMsgStartNowPlayingUpdates),
           "we advertise sending StartNowPlayingUpdates");
    expect(contains(received_blob, iap2::kMsgCarPlayAvailability),
           "we advertise receiving CarPlayAvailability");
    expect(contains(received_blob, iap2::kMsgNowPlayingUpdate), "we advertise receiving NowPlayingUpdate");
    expect(contains(received_blob, iap2::kMsgRouteGuidanceManeuverUpdate),
           "we advertise receiving RouteGuidanceManeuverUpdate");
    expect(!contains(sent_blob, iap2::kMsgIdentificationInformation),
           "identification itself is not in the advertised list");

    // Optional components can be dropped after a rejection.
    expect(iap2::csm::has(params, 22), "location information component is present by default");

    csm::ParamList rejection_params;
    iap2::csm::addNone(rejection_params, 22);
    const auto rejection = iap2::decodeIdentificationRejected(rejection_params);
    expect(rejection.has_value(), "IdentificationRejected decodes");
    expect(rejection && rejection->contains(22), "the rejection flags the location component");
    expect(rejection && rejection->flagged_names.size() == 1 &&
               rejection->flagged_names[0] == "location_information_component",
           "the rejection names the flagged field");

    expect(rejection && iap2::applyIdentificationRejection(*rejection, config),
           "a droppable rejection is applied");
    expect(!config.include_location_information, "the location component is dropped");

    const auto retried = iap2::csm::parseMessage(iap2::encodeIdentificationInformation(config));
    expect(retried && !iap2::csm::has(retried->params, 22),
           "the retried identification omits the location component");

    csm::ParamList fatal_params;
    iap2::csm::addNone(fatal_params, 0);
    const auto fatal = iap2::decodeIdentificationRejected(fatal_params);
    expect(fatal && !iap2::applyIdentificationRejection(*fatal, config),
           "a rejection of a mandatory field cannot be retried");
}

void testCarPlaySession()
{
    iap2::CarPlayStartSession session;
    session.ip_addresses = {"fe80::1c2:3ff:fe45:6789"};
    session.port = 7000;
    session.device_identifier = "aa:bb:cc:dd:ee:ff";
    session.public_key = "3q2+7w==";
    session.source_version = "410.35";

    const auto message = iap2::csm::parseMessage(iap2::encodeCarPlayStartSession(session));
    expect(message && message->id == iap2::kMsgCarPlayStartSession, "CarPlayStartSession encodes");
    if (message)
    {
        const auto wired = iap2::csm::getGroup(message->params, 0);
        expect(wired && iap2::csm::getString(*wired, 0) == "fe80::1c2:3ff:fe45:6789",
               "the accessory link local address round trips");
        expect(iap2::csm::getU32(message->params, 2) == 7000, "the AirPlay port round trips");
        expect(iap2::csm::getString(message->params, 3) == "aa:bb:cc:dd:ee:ff",
               "the Bluetooth MAC round trips");
        expect(iap2::csm::getString(message->params, 4) == "3q2+7w==", "the AirPlay public key round trips");
        expect(iap2::csm::getString(message->params, 5) == "410.35", "the source version round trips");
    }

    // CarPlayAvailability, wired branch.
    csm::ParamList wired_attributes;
    iap2::csm::addBool(wired_attributes, 0, true);
    iap2::csm::addString(wired_attributes, 1, "1122334455667788");
    csm::ParamList availability_params;
    iap2::csm::addGroup(availability_params, 0, wired_attributes);

    const auto availability = iap2::decodeCarPlayAvailability(availability_params);
    expect(availability && availability->has_wired, "CarPlayAvailability reports the wired attributes");
    expect(availability && availability->wired_available == true, "wired CarPlay is available");
    expect(availability && availability->usb_transport_identifier == "1122334455667788",
           "the USB transport identifier round trips");
    expect(availability && !availability->has_wireless, "no wireless attributes were sent");

    // DeviceTransportIdentifierNotification.
    csm::ParamList transport_params;
    iap2::csm::addString(transport_params, 0, "11:22:33:44:55:66");
    iap2::csm::addString(transport_params, 1, "0000deadbeef");
    const auto identifiers = iap2::decodeDeviceTransportIdentifierNotification(transport_params);
    expect(identifiers && identifiers->bluetooth_transport_id == "11:22:33:44:55:66",
           "the phone Bluetooth identifier decodes");
    expect(identifiers && identifiers->usb_transport_id == "0000deadbeef",
           "the phone USB identifier decodes");

    // WirelessCarPlayUpdate.
    csm::ParamList wireless_params;
    iap2::csm::addU8(wireless_params, 0, 1);
    expect(iap2::decodeWirelessCarPlayUpdate(wireless_params) == iap2::WirelessCarPlayStatus::kAvailable,
           "WirelessCarPlayUpdate decodes");
}

void testRouteGuidance()
{
    csm::ParamList guidance_params;
    iap2::csm::addU16(guidance_params, 0, 0);
    iap2::csm::addU8(guidance_params, 1, 1);
    iap2::csm::addU8(guidance_params, 2, 2);
    iap2::csm::addString(guidance_params, 3, "Hauptstrasse");
    iap2::csm::addString(guidance_params, 4, "Home");
    iap2::csm::addU64(guidance_params, 5, 1753000000ULL);
    iap2::csm::addU64(guidance_params, 6, 900ULL);
    iap2::csm::addU32(guidance_params, 7, 12000);
    iap2::csm::addU32(guidance_params, 10, 250);
    iap2::csm::addBytes(guidance_params, 13, {0x00, 0x07, 0x00, 0x08});
    const auto guidance = iap2::decodeRouteGuidanceUpdate(guidance_params);

    expect(guidance && guidance->state == 1, "route guidance state decodes");
    expect(guidance && guidance->maneuver_state == 2, "maneuver state decodes");
    expect(guidance && guidance->current_road_name == "Hauptstrasse", "current road name decodes");
    expect(guidance && guidance->destination_name == "Home", "destination name decodes");
    expect(guidance && guidance->eta == 1753000000ULL, "eta decodes");
    expect(guidance && guidance->time_remaining == 900ULL, "time remaining decodes");
    expect(guidance && guidance->distance_remaining == 12000u, "distance remaining decodes");
    expect(guidance && guidance->distance_to_maneuver == 250u, "distance to maneuver decodes");

    csm::ParamList maneuver_params;
    iap2::csm::addU16(maneuver_params, 1, 7);
    iap2::csm::addU8(maneuver_params, 3, 4);
    iap2::csm::addString(maneuver_params, 4, "Bahnhofstrasse");
    iap2::csm::addU8(maneuver_params, 8, 1);
    iap2::csm::addU8(maneuver_params, 9, 2);
    iap2::csm::addI16(maneuver_params, 11, -90);
    const auto maneuver = iap2::decodeRouteGuidanceManeuverUpdate(maneuver_params);

    expect(maneuver && maneuver->index == 7, "maneuver index decodes");
    expect(maneuver && maneuver->maneuver_type == 4, "maneuver type decodes");
    expect(maneuver && maneuver->after_maneuver_road_name == "Bahnhofstrasse",
           "after maneuver road name decodes");
    expect(maneuver && maneuver->exit_angle == -90, "a negative exit angle decodes");

    // Guidance first, then the maneuver.
    iap2::NavGuidance nav;
    nav.apply(*guidance);
    expect(nav.current_index == 7, "the current maneuver index comes from the maneuver list");
    expect(nav.road_name == "Hauptstrasse", "the merged view keeps the road name");
    expect(nav.remain_distance == 250u, "the merged view keeps the distance to the maneuver");
    nav.apply(*maneuver);
    expect(nav.maneuver_type == 4 && nav.turn_side == 1 && nav.junction_type == 2 && nav.turn_angle == -90,
           "the merged view picks up the current maneuver");
    expect(nav.after_road_name == "Bahnhofstrasse", "the merged view picks up the next road name");

    // Maneuver first, then the guidance that selects it.
    iap2::NavGuidance reordered;
    reordered.apply(*maneuver);
    expect(!reordered.maneuver_type.has_value(), "a maneuver alone does not become current");
    reordered.apply(*guidance);
    expect(reordered.maneuver_type == 4, "the guidance update selects an already known maneuver");

    // A maneuver update without an index is ignored.
    iap2::NavGuidance ignored;
    csm::ParamList indexless;
    iap2::csm::addU8(indexless, 3, 9);
    const auto no_index = iap2::decodeRouteGuidanceManeuverUpdate(indexless);
    ignored.apply(*no_index);
    expect(!ignored.maneuver_type.has_value(), "a maneuver update without an index is ignored");
}

void testCallAndPower()
{
    iap2::CallTracker tracker;
    expect(tracker.phase() == iap2::CallTracker::Phase::kEnded, "the call tracker starts idle");

    csm::ParamList ringing;
    iap2::csm::addString(ringing, 0, "+491234567");
    iap2::csm::addString(ringing, 1, "Alice");
    iap2::csm::addU8(ringing, 2, 2);
    iap2::csm::addU8(ringing, 3, 1);
    iap2::csm::addString(ringing, 4, "call-a");
    const auto ringing_state = iap2::decodeCallStateUpdate(ringing);
    expect(ringing_state && ringing_state->remote_id == "+491234567", "the caller number decodes");
    expect(ringing_state && ringing_state->display_name == "Alice", "the caller name decodes");
    expect(ringing_state && ringing_state->status == 2, "the call status decodes");

    expect(tracker.apply(*ringing_state), "an incoming call changes the phase");
    expect(tracker.phase() == iap2::CallTracker::Phase::kRinging, "the call is ringing");
    expect(tracker.name() == "Alice", "the ringing call name is exposed");

    csm::ParamList answered;
    iap2::csm::addU8(answered, 2, 1);
    iap2::csm::addString(answered, 4, "call-a");
    const auto answered_state = iap2::decodeCallStateUpdate(answered);
    expect(tracker.apply(*answered_state), "answering changes the phase");
    expect(tracker.phase() == iap2::CallTracker::Phase::kActive, "the call is active");
    expect(tracker.name() == "Alice", "the name survives an update that omits it");

    expect(!tracker.apply(*answered_state), "a repeated update does not report a change");

    csm::ParamList hung_up;
    iap2::csm::addU8(hung_up, 2, 0);
    iap2::csm::addString(hung_up, 4, "call-a");
    const auto hung_up_state = iap2::decodeCallStateUpdate(hung_up);
    expect(tracker.apply(*hung_up_state), "hanging up changes the phase");
    expect(tracker.phase() == iap2::CallTracker::Phase::kEnded, "the call has ended");
    expect(tracker.name().empty(), "the call name is cleared when the call ends");

    // Power.
    csm::ParamList power_params;
    iap2::csm::addU16(power_params, 0, 500);
    iap2::csm::addBool(power_params, 1, true);
    iap2::csm::addU8(power_params, 2, 1);
    iap2::csm::addBool(power_params, 4, true);
    iap2::csm::addU8(power_params, 5, 2);
    iap2::csm::addU16(power_params, 6, 87);
    const auto power = iap2::decodePowerUpdate(power_params);
    expect(power && power->battery_charge_level == 87, "the battery level decodes");
    expect(power && power->is_external_charger_connected == true, "the charger state decodes");
    expect(power && power->battery_charging_state == 2, "the charging state decodes");
    expect(power && power->maximum_current_drawn_from_accessory == 500, "the drawn current decodes");

    const auto source = iap2::csm::parseMessage(iap2::encodePowerSourceUpdate(2100, true));
    expect(source && source->id == iap2::kMsgPowerSourceUpdate, "PowerSourceUpdate encodes");
    expect(source && iap2::csm::getU16(source->params, 0) == 2100, "the advertised current round trips");
    expect(source && iap2::csm::getBool(source->params, 1) == true, "the charge permission round trips");

    // Cellular.
    csm::ParamList comms_params;
    iap2::csm::addBytes(comms_params, 0, {4, 5});
    iap2::csm::addString(comms_params, 4, "Telekom");
    iap2::csm::addBool(comms_params, 5, true);
    const auto cellular = iap2::decodeCommunicationsUpdate(comms_params);
    expect(cellular && cellular->signal_strength == 4, "the signal strength decodes");
    expect(cellular && cellular->carrier_name == "Telekom", "the carrier name decodes");
    expect(cellular && cellular->cellular_supported == true, "cellular support decodes");
}

void testSubscriptionMessages()
{
    const auto now_playing = iap2::csm::parseMessage(iap2::encodeStartNowPlayingUpdates());
    expect(now_playing && now_playing->id == iap2::kMsgStartNowPlayingUpdates,
           "StartNowPlayingUpdates encodes");
    if (now_playing)
    {
        const auto media = iap2::csm::getGroup(now_playing->params, 0);
        const auto playback = iap2::csm::getGroup(now_playing->params, 1);
        expect(media && iap2::csm::has(*media, 1) && iap2::csm::has(*media, 12) &&
                   iap2::csm::has(*media, 6) && iap2::csm::has(*media, 4) && iap2::csm::has(*media, 26),
               "we subscribe to title, artist, album, duration and artwork");
        expect(playback && iap2::csm::has(*playback, 0) && iap2::csm::has(*playback, 1) &&
                   iap2::csm::has(*playback, 7),
               "we subscribe to status, elapsed time and app name");
        expect(media && iap2::csm::find(*media, 1)->data.empty(),
               "subscription parameters are none-like (zero length)");
    }

    const auto power = iap2::csm::parseMessage(iap2::encodeStartPowerUpdates());
    expect(power && iap2::csm::has(power->params, 6) && iap2::csm::has(power->params, 5) &&
               iap2::csm::has(power->params, 4),
           "we subscribe to battery level, charging state and charger presence");

    const auto calls = iap2::csm::parseMessage(iap2::encodeStartCallStateUpdates());
    expect(calls && calls->params.size() == 6, "we subscribe to the six call state fields LIVI asks for");

    const auto comms = iap2::csm::parseMessage(iap2::encodeStartCommunicationsUpdates());
    expect(comms && comms->params.size() == 3, "we subscribe to signal, carrier and cellular support");

    const auto guidance = iap2::csm::parseMessage(iap2::encodeStartRouteGuidanceUpdates());
    expect(guidance && guidance->params.empty(),
           "StartRouteGuidanceUpdates has no display component filter");
    expect(guidance && guidance->id == iap2::kMsgStartRouteGuidanceUpdates,
           "StartRouteGuidanceUpdates has the right id");

    const auto vehicle = iap2::csm::parseMessage(iap2::encodeVehicleStatusUpdate(420, -5, true));
    expect(vehicle && iap2::csm::getU16(vehicle->params, 3) == 420, "the vehicle range round trips");
    expect(vehicle && iap2::csm::getI16(vehicle->params, 4) == -5,
           "a negative outside temperature round trips");
    expect(vehicle && iap2::csm::getBool(vehicle->params, 6) == true, "the range warning round trips");

    const auto location = iap2::csm::parseMessage(iap2::encodeLocationInformation("$GPGGA,123519,,,,"));
    expect(location && iap2::csm::getString(location->params, 0) == "$GPGGA,123519,,,,",
           "an NMEA sentence round trips");

    const auto wifi = iap2::csm::parseMessage(iap2::encodeAccessoryWiFiConfigurationInformation(
        "LIVI", "secret", iap2::WiFiSecurityType::kWpaWpa2, 6));
    expect(wifi && iap2::csm::getString(wifi->params, 1) == "LIVI", "the SSID round trips");
    expect(wifi && iap2::csm::getString(wifi->params, 2) == "secret", "the passphrase round trips");
    expect(wifi && iap2::csm::getU8(wifi->params, 3) == 2, "the security type round trips");
    expect(wifi && iap2::csm::getU8(wifi->params, 4) == 6, "the channel round trips");
}

void testMfiAuthentication()
{
    StubSigner signer(3);
    iap2::MfiAuthenticator authenticator(signer);
    std::vector<uint8_t> reply;

    // Certificate request.
    auto request = iap2::csm::parseMessage(
        iap2::csm::encodeMessage(iap2::kMsgRequestAuthenticationCertificate, {}));
    expect(request.has_value(), "RequestAuthenticationCertificate parses");
    expect(authenticator.handle(*request, reply) == iap2::MfiAuthenticator::Result::kReply,
           "a certificate request produces a reply");

    auto certificate_message = iap2::csm::parseMessage(reply);
    expect(certificate_message && certificate_message->id == iap2::kMsgAuthenticationCertificate,
           "the reply is an AuthenticationCertificate");
    expect(certificate_message &&
               iap2::csm::getBytes(certificate_message->params, 0) == std::vector<uint8_t>(300, 0xC7),
           "the coprocessor certificate is forwarded verbatim");

    // Challenge, SHA-256 sized for protocol major 3.
    const std::vector<uint8_t> challenge(32, 0x11);
    csm::ParamList challenge_params;
    iap2::csm::addBytes(challenge_params, 0, challenge);
    auto challenge_message = iap2::csm::parseMessage(
        iap2::csm::encodeMessage(iap2::kMsgRequestAuthenticationChallengeResponse, challenge_params));
    expect(challenge_message.has_value(), "RequestAuthenticationChallengeResponse parses");
    expect(iap2::decodeAuthenticationChallenge(challenge_message->params) == challenge,
           "the challenge decodes");

    expect(authenticator.handle(*challenge_message, reply) == iap2::MfiAuthenticator::Result::kReply,
           "a challenge produces a reply");
    expect(signer.last_challenge == challenge, "the challenge reaches the signer untouched");

    auto response_message = iap2::csm::parseMessage(reply);
    expect(response_message && response_message->id == iap2::kMsgAuthenticationResponse,
           "the reply is an AuthenticationResponse");
    expect(response_message &&
               iap2::csm::getBytes(response_message->params, 0) == std::vector<uint8_t>(128, 0x5A),
           "the signature is forwarded verbatim");

    // Terminal states.
    auto succeeded =
        iap2::csm::parseMessage(iap2::csm::encodeMessage(iap2::kMsgAuthenticationSucceeded, {}));
    expect(authenticator.handle(*succeeded, reply) == iap2::MfiAuthenticator::Result::kSucceeded,
           "AuthenticationSucceeded ends the exchange");

    auto failed = iap2::csm::parseMessage(iap2::csm::encodeMessage(iap2::kMsgAuthenticationFailed, {}));
    expect(authenticator.handle(*failed, reply) == iap2::MfiAuthenticator::Result::kFailed,
           "AuthenticationFailed is reported");

    auto unrelated = iap2::csm::parseMessage(iap2::encodeStartPowerUpdates());
    expect(authenticator.handle(*unrelated, reply) == iap2::MfiAuthenticator::Result::kIgnored,
           "unrelated messages are ignored by the authenticator");

    // Protocol major 2 wants a SHA-1 sized challenge; a mismatch is signed
    // anyway but must not crash.
    StubSigner sha1_signer(2);
    iap2::MfiAuthenticator sha1_authenticator(sha1_signer);
    expect(sha1_authenticator.handle(*challenge_message, reply) == iap2::MfiAuthenticator::Result::kReply,
           "a challenge size mismatch still produces a reply");
}

// ---------------------------------------------------------------------------
// End to end: identification and authentication over the link layer.
// ---------------------------------------------------------------------------
void testHandshakeOverLink()
{
    FakeTransport transport;
    iap2::LinkLayer link(transport, iap2::LinkConfig{});
    uint8_t device_seq = negotiate(link, transport, deviceLsp(4, 65535, 0, 0, 0, 0));
    drainSent(transport);

    const auto pushCsm = [&](const std::vector<uint8_t>& frame)
    {
        device_seq = static_cast<uint8_t>(device_seq + 1);
        transport.push(buildPacket(iap2::kControlAck, device_seq, link.sentPsn(), iap2::kControlSessionId,
                                   frame));
    };

    // StartIdentification -> IdentificationInformation -> IdentificationAccepted.
    pushCsm(iap2::csm::encodeMessage(iap2::kMsgStartIdentification, {}));
    auto incoming = link.receiveControlMessage(50);
    auto message = incoming ? iap2::csm::parseMessage(*incoming) : std::nullopt;
    expect(message && message->id == iap2::kMsgStartIdentification, "StartIdentification arrives");

    link.sendControlMessage(iap2::encodeIdentificationInformation(iap2::IdentificationConfig{}));
    auto packets = drainSent(transport);
    expect(!packets.empty(), "identification information goes out");

    pushCsm(iap2::csm::encodeMessage(iap2::kMsgIdentificationAccepted, {}));
    incoming = link.receiveControlMessage(50);
    message = incoming ? iap2::csm::parseMessage(*incoming) : std::nullopt;
    expect(message && message->id == iap2::kMsgIdentificationAccepted, "IdentificationAccepted arrives");

    // Authentication, driven by the MFi helper.
    StubSigner signer(2);
    iap2::MfiAuthenticator authenticator(signer);
    std::vector<uint8_t> reply;

    pushCsm(iap2::csm::encodeMessage(iap2::kMsgRequestAuthenticationCertificate, {}));
    incoming = link.receiveControlMessage(50);
    message = incoming ? iap2::csm::parseMessage(*incoming) : std::nullopt;
    expect(message && authenticator.handle(*message, reply) == iap2::MfiAuthenticator::Result::kReply,
           "the certificate request is handled");
    link.sendControlMessage(reply);

    // The 300 byte certificate fits in one packet at max_len 65535.
    packets = drainSent(transport);
    expect(packets.size() == 1, "the certificate goes out in one packet");
    if (!packets.empty())
    {
        expect(packets[0].payload == reply, "the certificate message is transmitted intact");
    }

    csm::ParamList challenge_params;
    iap2::csm::addBytes(challenge_params, 0, std::vector<uint8_t>(20, 0x22));
    pushCsm(iap2::csm::encodeMessage(iap2::kMsgRequestAuthenticationChallengeResponse, challenge_params));
    incoming = link.receiveControlMessage(50);
    message = incoming ? iap2::csm::parseMessage(*incoming) : std::nullopt;
    expect(message && authenticator.handle(*message, reply) == iap2::MfiAuthenticator::Result::kReply,
           "the challenge is handled");
    expect(signer.last_challenge.size() == 20, "a protocol major 2 challenge is 20 bytes");
    link.sendControlMessage(reply);
    drainSent(transport);

    pushCsm(iap2::csm::encodeMessage(iap2::kMsgAuthenticationSucceeded, {}));
    incoming = link.receiveControlMessage(50);
    message = incoming ? iap2::csm::parseMessage(*incoming) : std::nullopt;
    expect(message && authenticator.handle(*message, reply) == iap2::MfiAuthenticator::Result::kSucceeded,
           "authentication completes over the link");

    // Handler based delivery.
    int delivered = 0;
    link.setControlMessageHandler([&](const std::vector<uint8_t>& frame)
                                  {
                                      const auto parsed = iap2::csm::parseMessage(frame);
                                      if (parsed && parsed->id == iap2::kMsgPowerUpdate)
                                      {
                                          ++delivered;
                                      }
                                  });
    csm::ParamList power_params;
    iap2::csm::addU16(power_params, 6, 55);
    pushCsm(iap2::csm::encodeMessage(iap2::kMsgPowerUpdate, power_params));
    link.poll(0);
    expect(delivered == 1, "a registered handler receives inbound control messages");
}

void testTransportFailure()
{
    FakeTransport transport;
    iap2::LinkLayer link(transport, iap2::LinkConfig{});
    transport.fail_sends = true;
    link.start();
    expect(!link.alive(), "a transport write failure kills the link");
    expect(!link.sendControlMessage(iap2::encodeStartPowerUpdates()),
           "sending on a dead link fails rather than throwing");
    expect(!link.poll(0), "poll reports a dead link");
}

void testExternalAccessoryAndFileTransfer()
{
    FakeTransport transport;
    iap2::LinkLayer link(transport, iap2::LinkConfig{});
    uint8_t device_seq = negotiate(link, transport, deviceLsp(4, 65535, 0, 0, 0, 0));
    drainSent(transport);

    uint16_t seen_stream = 0;
    std::vector<uint8_t> seen_ea;
    std::vector<uint8_t> seen_file;
    link.setExternalAccessoryHandler([&](uint16_t stream_id, const std::vector<uint8_t>& data)
                                     {
                                         seen_stream = stream_id;
                                         seen_ea = data;
                                     });
    link.setFileTransferHandler([&](const std::vector<uint8_t>& data) { seen_file = data; });

    std::vector<uint8_t> ea_payload = {0x00, 0x2A};
    const std::vector<uint8_t> ea_body = asBytes("hello ea");
    ea_payload.insert(ea_payload.end(), ea_body.begin(), ea_body.end());
    device_seq = static_cast<uint8_t>(device_seq + 1);
    transport.push(buildPacket(iap2::kControlAck, device_seq, 99, iap2::kExternalAccessorySessionId,
                               ea_payload));
    link.poll(0);
    expect(seen_stream == 42, "the external accessory stream id is unwrapped");
    expect(seen_ea == ea_body, "the external accessory payload is delivered without the stream id");

    const std::vector<uint8_t> file_body = asBytes("artwork bytes");
    device_seq = static_cast<uint8_t>(device_seq + 1);
    transport.push(buildPacket(iap2::kControlAck, device_seq, 99, iap2::kFileTransferSessionId, file_body));
    link.poll(0);
    expect(seen_file == file_body, "file transfer payloads reach their handler");

    // Outbound framing puts the stream id back on the front.
    link.sendExternalAccessory(42, ea_body);
    const auto packets = drainSent(transport);
    expect(packets.size() == 1 && packets[0].header.session_id == iap2::kExternalAccessorySessionId,
           "external accessory data goes out on session 11");
    if (!packets.empty())
    {
        expect(packets[0].payload == ea_payload, "the outbound stream id is prefixed big endian");
    }
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    testChecksums();
    testHeaderRoundTrip();
    testLspRoundTrip();
    testStartAndNegotiation();
    testControlSessionDataPath();
    testReassembly();
    testFragmentedSend();
    testOutOfSequence();
    testEakRetransmission();
    testQueuedBeforeNegotiation();
    testCsmFraming();
    testIdentification();
    testCarPlaySession();
    testRouteGuidance();
    testCallAndPower();
    testSubscriptionMessages();
    testMfiAuthentication();
    testHandshakeOverLink();
    testTransportFailure();
    testExternalAccessoryAndFileTransfer();

    if (failures == 0)
    {
        SPDLOG_INFO("iap2 framing tests passed");
        return EXIT_SUCCESS;
    }
    SPDLOG_ERROR("{} iap2 framing test(s) failed", failures);
    return EXIT_FAILURE;
}
