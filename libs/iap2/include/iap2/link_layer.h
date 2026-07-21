// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/link_layer.py
#ifndef IAP2_LINK_LAYER_H_
#define IAP2_LINK_LAYER_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace iap2
{

// ---------------------------------------------------------------------------
// Byte transport beneath the iAP2 link layer.
//
// The link layer never sees USB, Bluetooth or TLS; a transport just moves
// bytes. `apple_usb::CarkitChannel` (the lockdown TLS channel to a wired
// iPhone) has exactly this shape and can be adapted with a two-line wrapper --
// deliberately not referenced here so this library stays dependency free.
//
// recv() must return an empty vector when it times out with no data, and is
// also allowed to return short reads. A transport that has died should report
// that through its own channel; the link layer treats an empty read as "no
// data yet".
// ---------------------------------------------------------------------------
class Iap2Transport
{
  public:
    virtual ~Iap2Transport() = default;

    // Blocking write of the whole buffer. Returns false on error/closed.
    virtual bool send(const uint8_t* data, size_t len) = 0;

    // Blocking read of up to max_len bytes. Returns empty on timeout/EOF.
    virtual std::vector<uint8_t> recv(size_t max_len, unsigned timeout_ms) = 0;
};

// Link packet control bits.
constexpr uint8_t kControlSyn = 0x80;
constexpr uint8_t kControlAck = 0x40;
constexpr uint8_t kControlEak = 0x20;
constexpr uint8_t kControlRst = 0x10;

constexpr uint16_t kLinkPacketStart = 0xFF5A;
constexpr size_t kLinkHeaderSize = 9;  // 8 bytes + header checksum

// Session identifiers advertised in the link synchronisation payload.
constexpr uint8_t kControlSessionId = 10;
constexpr uint8_t kExternalAccessorySessionId = 11;
constexpr uint8_t kFileTransferSessionId = 12;

// The "iAP2 supported" detection marker the accessory sends before negotiating.
inline constexpr uint8_t kIap2Marker[] = {0xFF, 0x55, 0x02, 0x00, 0xEE, 0x10};
constexpr size_t kIap2MarkerSize = sizeof(kIap2Marker);

// iAP2 checksum: the bytes of the region plus the checksum byte sum to zero
// modulo 256.
uint8_t genChecksum(const uint8_t* data, size_t len);
bool checkChecksum(const uint8_t* data, size_t len);

struct LinkPacketHeader
{
    // Total packet length: 9 for a control-only packet, payload + 10 otherwise.
    uint16_t length = 0;
    uint8_t control = 0;
    uint8_t seq = 0;
    uint8_t ack = 0;
    uint8_t session_id = 0;

    // 9 bytes: start marker, length, control, seq, ack, session, checksum.
    std::vector<uint8_t> pack() const;

    // Validates the start marker and the header checksum.
    static std::optional<LinkPacketHeader> parse(const uint8_t* data, size_t len);
};

struct LinkSession
{
    uint8_t id = 0;
    uint8_t type = 0;
    uint8_t version = 0;

    bool operator==(const LinkSession& other) const = default;
};

// Payload of a SYN packet: the link parameters each side is willing to use,
// followed by the session table.
struct LinkSynchronizationPayload
{
    static constexpr uint8_t kVersion = 0x01;
    static constexpr size_t kFixedSize = 10;

    uint8_t max_outgoing = 4;
    uint16_t max_len = 65535;
    uint16_t retransmission_timeout = 0;
    uint16_t ack_timeout = 0;
    uint8_t max_retransmissions = 0;
    uint8_t max_ack = 0;
    std::vector<LinkSession> sessions;

    std::vector<uint8_t> pack() const;
    static std::optional<LinkSynchronizationPayload> parse(const uint8_t* data, size_t len);
};

// Wired ("carkit") defaults: zero_ack, control session version 2, and we drive
// the negotiation ourselves rather than waiting for the phone's marker.
struct LinkConfig
{
    uint8_t max_outgoing = 4;
    bool zero_ack = true;
    uint8_t control_version = 2;
    bool initiate_negotiate = true;
    uint16_t ack_timeout_ms = 500;
    uint8_t max_outgoing_delta = 0;
    std::string tag = "usb";
};

// ---------------------------------------------------------------------------
// The iAP2 link layer.
//
// Single threaded and non-owning of its transport: the caller drives it with
// poll() (or one of the blocking helpers, which call poll internally). All
// callbacks fire on the calling thread from inside poll().
// ---------------------------------------------------------------------------
class LinkLayer
{
  public:
    enum class State
    {
        kIdle,
        kDetectIap2Support,
        kNegotiate,
        kNormal,
        kDead,
    };

    // A complete control session message, CSM header included.
    using ControlMessageHandler = std::function<void(const std::vector<uint8_t>& message)>;
    using FileTransferHandler = std::function<void(const std::vector<uint8_t>& data)>;
    using ExternalAccessoryHandler = std::function<void(uint16_t stream_id, const std::vector<uint8_t>& data)>;

    LinkLayer(Iap2Transport& transport, LinkConfig config = {});

    // Inbound control session messages are queued; if a handler is installed
    // poll() drains the queue into it instead of leaving them for
    // receiveControlMessage().
    void setControlMessageHandler(ControlMessageHandler handler);
    void setFileTransferHandler(FileTransferHandler handler);
    void setExternalAccessoryHandler(ExternalAccessoryHandler handler);

    // Sends the detection marker and, when initiate_negotiate is set, the SYN.
    bool start();

    // Pumps the transport for at most timeout_ms, processing whatever arrives
    // and firing due timers. Returns false once the link is dead.
    bool poll(unsigned timeout_ms);

    // Pumps until the link reaches the normal state. Returns false on timeout
    // or link death.
    bool waitNegotiated(unsigned timeout_ms);

    // Pops the next inbound control session message, pumping the transport
    // until one arrives or the timeout expires.
    std::optional<std::vector<uint8_t>> receiveControlMessage(unsigned timeout_ms);

    // `message` is a complete CSM frame (see messages.h). Queued if the link is
    // not negotiated yet, and split across packets if it exceeds the negotiated
    // maximum packet length.
    bool sendControlMessage(const std::vector<uint8_t>& message);
    bool sendFileTransfer(const std::vector<uint8_t>& data);
    bool sendExternalAccessory(uint16_t stream_id, const std::vector<uint8_t>& data);

    void close();

    State state() const { return state_; }
    bool negotiated() const { return state_ == State::kNormal; }
    bool alive() const { return state_ != State::kDead; }
    const LinkSynchronizationPayload& linkParameters() const { return lsp_; }

    // Largest payload we will put in a single link packet.
    size_t maxPayload() const;

    // Sequence number of the last packet we sent, for tests and logging.
    uint8_t sentPsn() const { return sent_psn_; }
    uint8_t lastReceivedPsn() const { return last_received_in_sequence_psn_; }

  private:
    using Clock = std::chrono::steady_clock;

    struct OutPacket
    {
        std::vector<uint8_t> data;
        uint8_t session_id = 0;
        uint8_t psn = 0;
        unsigned counter = 0;
        Clock::time_point timeout{};
    };

    struct InPacket
    {
        std::vector<uint8_t> data;
        uint8_t session_id = 0;
        uint8_t psn = 0;
    };

    void writePacket(const uint8_t* payload, size_t payload_len, uint8_t seq, uint8_t control,
                     uint8_t session_id);
    void sendAck();
    void sendEak(const std::vector<uint8_t>& psns);
    void sendData(const OutPacket& packet);
    void sendDetectMarker();
    void sendNegotiate();

    bool sendSessionData(uint8_t session_id, const uint8_t* data, size_t len);
    void enqueuePacket(OutPacket packet);

    void processRxBuffer();
    void handlePacket(const LinkPacketHeader& header, const std::optional<std::vector<uint8_t>>& payload);
    void handleSyn(const LinkSynchronizationPayload& lsp, uint8_t psn);
    void handleAck(uint8_t ack);
    void handleEak(const std::vector<uint8_t>& psns);
    void handleData(const std::vector<uint8_t>& payload, uint8_t psn, uint8_t session_id);
    void deliver(const std::vector<uint8_t>& payload, uint8_t session_id);
    void parseControlSession();

    void fireTimers();
    void onExpectAckTimer();
    void onSendAckTimer();
    void disarmSendAckTimer() { send_ack_deadline_.reset(); }
    void rearmSendAckTimer();
    void disarmRecvAckTimer() { recv_ack_deadline_.reset(); }
    void rearmRecvAckTimer(Clock::time_point deadline);

    void bailout(const char* reason);
    void drainControlQueue();

    Iap2Transport& transport_;
    LinkConfig config_;
    State state_ = State::kIdle;
    LinkSynchronizationPayload lsp_;

    // Outgoing sequencing. LIVI starts at 99 so the first data packet is 100.
    uint8_t sent_psn_ = 99;
    int last_sent_acknowledged_psn_ = -1;
    std::vector<OutPacket> unacked_;
    std::deque<OutPacket> queued_;

    // Incoming sequencing.
    uint8_t last_received_in_sequence_psn_ = 0;
    int last_acked_psn_ = -1;
    std::vector<InPacket> received_out_of_sequence_;
    unsigned cumulative_received_ = 0;

    // Raw transport bytes not yet consumed by the framer.
    std::vector<uint8_t> rx_;
    size_t rx_head_ = 0;

    // Control session byte stream, reassembled into whole CSM frames.
    std::vector<uint8_t> control_rx_;
    std::deque<std::vector<uint8_t>> control_queue_;

    ControlMessageHandler control_handler_;
    FileTransferHandler file_transfer_handler_;
    ExternalAccessoryHandler external_accessory_handler_;

    std::optional<Clock::time_point> marker_deadline_;
    std::optional<Clock::time_point> negotiate_deadline_;
    std::optional<Clock::time_point> send_ack_deadline_;
    std::optional<Clock::time_point> recv_ack_deadline_;
};

// Circular distance between two sequence numbers, as LIVI's distance().
uint8_t sequenceDistance(uint8_t a, int b);

}  // namespace iap2

#endif  // IAP2_LINK_LAYER_H_
