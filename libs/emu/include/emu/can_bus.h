// A CAN bus: the frames between the nodes on it.
//
// Every node (a simulated controller, a scenario's tester, the outside world's node) is attached once. A node puts a
// frame on the bus with send(); the bus passes it through the tap and hands it to every other node in the order they
// were attached, which fixes the order in which one frame reaches its receivers. Like every node here the bus is
// instantaneous: when a controller models bus time (arbitration, error frames) it does so on its own
// side of the connection.
//
// The tap is the bus-level fault point: it sees every frame sent and may change it or drop it. It is empty unless a
// fault is attached; an empty tap costs one test.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "emu/common.h"
#include "helpers/can_frame.h"

namespace sim::sys {

// The frame between simulated controllers is the tree's own, so what a
// simulated device puts on its bus goes onto a real one, or onto zenoh, as it is.
// A classic controller carries `len` 0..8; a DLC of 9..15 on the wire, which
// still means eight bytes, is not distinguished from 8.
using CanFrame = helpers::CanFrame;

// The bytes a classic frame carries: none for a remote frame.
inline uint8_t classic_payload_bytes(const CanFrame& frame) {
    return frame.isRTR ? uint8_t{0} : std::min<uint8_t>(frame.len, 8);
}

// The error a faulty node's flag or a disturbance causes, as the controllers classify it.
enum class BusErrorKind : uint8_t { Bit, Stuff, Form, Crc };

// A physical fault of the bus wire from `start_s` to `end_s` (seconds), as the ECU's controller sees it:
//   Open        the ECU's stub is cut or no other node is powered: nothing received, no ACK for its frames (ACK
//               error; TEC + 8 up to error passive, no further: the ACK-error exception of the passive
//               transmitter; never bus off)
//   Recessive   CAN_H shorted to CAN_L: no node can drive dominant; the ECU's start of frame reads recessive (bit 0
//               error, TEC + 8 per attempt, + 8 per failed bit of an active error flag) -> bus off within about 1 ms
//               of a frame; the bus stays recessive, so the 11-bit sequences of bus-off recovery are seen
//   Dominant    the bus stuck dominant (a line shorted), beginning while the ECU sends a frame (its next one at or
//               after `start_s`, in the data field): bit 1 error, then + 8 per 8 dominant bits -> bus off; no
//               recessive bits, so TXECTR stays 0 while it lasts; nothing received or sent
//   DominantRx  the same short beginning while the ECU receives or idles: it loses arbitration to the dominant
//               level, a stuff error, + 8 per 8 dominant bits on REC -> error passive (REC above 127 is not counted
//               further); no bus off
//   Errors      a faulty node sends error flags at `error_flags_per_s` (random times, exponential gaps from a fixed
//               seed: periodic ones would alias with the periodic traffic): the frame on the bus at that moment is
//               destroyed (the ECU as transmitter: `error` maps to a bit / stuff / form error, TEC + 8; as receiver:
//               stuff / form / CRC error, REC + 1) and retransmitted; on an idle bus the flag is a start of frame and
//               a stuff error (REC + 1)
//   Disturb     a transient disturbance: every frame overlapping the window is destroyed (as Errors) and
//               retransmitted
struct BusFault {
    enum class Mode : uint8_t { Open, Recessive, Dominant, DominantRx, Errors, Disturb };
    Mode mode = Mode::Open;
    BusErrorKind error = BusErrorKind::Bit;  // Errors / Disturb
    double start_s = 0;
    double end_s = 1e18;
    double error_flags_per_s = 100;  // Errors
};

// How a node's frames wait for the bus while it is busy (a controller that models bus time asks the sender): one
// transmit buffer per id, a newer frame replacing a waiting one of the same id (a node with a mailbox per periodic
// message), or every frame in order (a host adapter's transmit queue).
enum class CanSenderQueue : uint8_t { OneFramePerId, InOrder };

// Whose transmitter sends one of a node's frames, as fault confinement counts it (a controller that models the other
// nodes' error counters asks the sender, CanNode::transmitter_of):
//   PerId  one transmitter per frame id: a node standing in for ECUs whose sender of that id is not known
//   Node   the node's own controller, for all its frames (a host adapter)
//   Ecu    the ECU `ecu` the node stands in for (a scenario's sender, by the CAN matrix): one transmitter per ECU
struct CanTransmitter {
    enum class Kind : uint8_t { PerId, Node, Ecu };
    Kind kind = Kind::PerId;
    uint16_t ecu = 0;
};

class CanNode {
   public:
    // A frame another node put on the bus at sim time `now`. Returns true if this node took it (a controller with a
    // free matching buffer); nodes that only look at the traffic return false.
    virtual bool receive(const CanFrame& frame, const CanNode& from, uint64_t now) = 0;

    virtual CanSenderQueue sender_queue() const {
        return CanSenderQueue::OneFramePerId;
    }

    virtual CanTransmitter transmitter_of(const CanFrame& frame) const {
        (void)frame;
        return {};
    }

    // The bus wire's physical faults changed (CanBus::set_faults): a controller that models bus time reads them.
    virtual void wire_faults_changed(const std::vector<BusFault>& faults) {
        (void)faults;
    }

   protected:
    ~CanNode() = default;
};

class CanBus {
   public:
    // The frame as the nodes see it, for the frame the sender put on the bus. Return false to drop it.
    using Tap = std::function<bool(CanFrame& frame, uint64_t now)>;

    // Nodes are not owned and must outlive the bus's use; the order of attach() is the order of delivery.
    void attach(CanNode& node) {
        nodes_.push_back(&node);

        if (!faults_.empty()) {
            node.wire_faults_changed(faults_);
        }
    }

    // The wire's physical faults (the scenario's "canbus" injections), told to every node. They act on the
    // controllers' view of the bus (arbitration, error counts, bus time), not on the frames.
    void set_faults(std::vector<BusFault> faults) {
        faults_ = std::move(faults);

        for (CanNode* node : nodes_) {
            node->wire_faults_changed(faults_);
        }
    }

    // `from` puts a frame on the bus: through the tap, then to every other node. True if some node took it.
    bool send(const CanNode& from, CanFrame frame, uint64_t now) const {
        if (tap && !tap(frame, now)) {
            return false;
        }

        bool taken = false;

        for (CanNode* node : nodes_) {
            if (node != &from && node->receive(frame, from, now)) {
                taken = true;
            }
        }

        return taken;
    }

    // The node's place in the order of attach() (a stable name for it in snapshots); -1 if not attached.
    int index_of(const CanNode& node) const {
        for (size_t k = 0; k < nodes_.size(); k++) {
            if (nodes_[k] == &node) {
                return int(k);
            }
        }

        return -1;
    }

    Tap tap;

   private:
    std::vector<CanNode*> nodes_;
    std::vector<BusFault> faults_;
};

}  // namespace sim::sys
