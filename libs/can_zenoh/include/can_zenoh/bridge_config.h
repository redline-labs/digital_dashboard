#ifndef CAN_ZENOH_BRIDGE_CONFIG_H_
#define CAN_ZENOH_BRIDGE_CONFIG_H_

#include <cstdint>
#include <string>

namespace can_zenoh
{

// What a bridged channel is called and where its traffic goes. The hardware
// side -- bit rates, listen-only, queue depth -- is whoever opened the channel's
// business; a simulated device has none of it.
struct BridgeConfig
{
    // How this bus is referred to everywhere above the hardware: the `channel`
    // field of every frame published, and the status entry.
    std::string name;

    // What the channel was opened from, for the status topic and the trace
    // header. Descriptive only; nothing here opens it.
    std::string device;

    // Where frames go and come from.
    std::string rxKey;
    std::string txKey;

    // Publish received frames at all. Off for a channel that exists only to
    // transmit, where publishing would echo a diagnostic tool's own traffic
    // back at it.
    bool publishRx { true };
    // Accept frames from `txKey` and put them on the bus. Off is a stronger
    // statement than listen-only: nothing can even ask.
    bool acceptTx { true };

    // Write everything seen on this channel -- received and transmitted -- to a
    // PCAN .trc file at this path, readable by PCAN-Explorer and PCAN-View.
    // Empty records nothing.
    //
    // This is a tap on the channel rather than a `trc:` device because a
    // send() only ever sees what this process transmits, and a trace is worth
    // keeping precisely because it has both directions in it.
    std::string recordTrcPath;

    // What goes in the trace's Bus column, so several channels recorded from
    // one process stay distinguishable in the files they produce.
    uint8_t recordTrcBus { 1 };

    // The program named in the trace's header.
    std::string recordGeneratedBy { "Redline can_bridge" };
};

}  // namespace can_zenoh

#endif  // CAN_ZENOH_BRIDGE_CONFIG_H_
