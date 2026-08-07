@0xb4e17a2c9d3f6081;

# Controlling and observing the CAN bridge.

# Ask a channel to change bit rate.
#
# This is not a cheap operation and it is not local: the controller goes into
# reset, the interface comes down and back up, and on SocketCAN it needs
# CAP_NET_ADMIN. Anything mid-frame on the bus when it happens will see an
# error. It is here because a bench session where the bus rate is unknown is
# otherwise a node restart per guess.
struct CanBridgeSetBitrateRequest {
  # Which channel, by the name it was given in the config -- "can0", "chassis".
  # Not the hardware id: the point of the name is that nothing above the config
  # has to change when the hardware does.
  channel @0 :Text;

  # The arbitration-phase rate, in bit/s. Required.
  nominalBps @1 :UInt32;

  # The CAN FD data-phase rate, in bit/s. Zero means classic CAN: no data
  # phase, no bit-rate switching, eight bytes maximum.
  dataBps @2 :UInt32;

  # Where in the bit to sample, in per mille (875 = 87.5%). Zero asks for the
  # CiA default for the rate, which is where everything else on a vehicle bus
  # will be sampling -- override it only to match something that is not.
  nominalSamplePointPermille @3 :UInt16;
  dataSamplePointPermille @4 :UInt16;
}

struct CanBridgeSetBitrateResponse {
  ok @0 :Bool;

  # Empty when ok. Otherwise why not: a rate the controller cannot generate
  # from its clock, a missing privilege, a channel that is not configured.
  error @1 :Text;

  # What the channel is actually running at now. On success this is what was
  # asked for; on failure it is what it was left at, which is the thing worth
  # knowing before deciding whether to retry.
  actualNominalBps @2 :UInt32;
  actualDataBps @3 :UInt32;
  actualNominalSamplePointPermille @4 :UInt16;
}

# The CAN error states, in the order a controller walks up them as errors
# accumulate. A node drops off the bus entirely at the end.
enum CanBusState {
  unknown @0;
  errorActive @1;
  errorWarning @2;
  errorPassive @3;
  busOff @4;
  stopped @5;
}

struct CanBridgeChannelStatus {
  # The configured name, and the hardware it resolved to.
  name @0 :Text;
  device @1 :Text;
  description @2 :Text;

  open @3 :Bool;
  running @4 :Bool;
  # Empty unless the channel failed to open, or has failed since.
  error @5 :Text;

  nominalBps @6 :UInt32;
  dataBps @7 :UInt32;
  listenOnly @8 :Bool;

  state @9 :CanBusState;

  rxFrames @10 :UInt64;
  txFrames @11 :UInt64;

  # Frames lost because a queue filled or the adapter reported its own overrun.
  # Non-zero means the capture has holes in it, which is the difference between
  # a trace you can reason about and one you cannot.
  rxDropped @12 :UInt64;
  txDropped @13 :UInt64;

  errorFrames @14 :UInt64;
  busOffCount @15 :UInt64;
  rxErrorCounter @16 :UInt8;
  txErrorCounter @17 :UInt8;

  # Where this channel is being written as a PCAN .trc trace, empty when it is
  # not being recorded.
  recordPath @18 :Text;

  # Records written to that file, and records the recorder had to drop because
  # its queue filled -- a disk that cannot keep up with the bus. Same reasoning
  # as rxDropped above: a trace with a hole is worth having as long as you know
  # the hole is there, and worthless if you do not.
  recordedFrames @19 :UInt64;
  recordDropped @20 :UInt64;
}

# What every configured channel is doing. Published on a change and on a timer,
# so a late subscriber does not have to wait for something to go wrong to learn
# the state of the bus.
struct CanBridgeStatus {
  channels @0 :List(CanBridgeChannelStatus);
}
