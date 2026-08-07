@0x9cf0e7f8b3b8a7b1;

# One CAN frame on the wire.
#
# Fields 3 and up were added when real adapters arrived. Cap'n Proto field
# additions are backward compatible in both directions, so a publisher that
# predates them (the CANopen nodes) leaves them at their defaults and a
# subscriber that predates them ignores them. Nothing else had to change.
#
# What they are for: without `extended` an 11-bit identifier 0x123 and a 29-bit
# identifier 0x123 are the same message here and different messages on the bus,
# and a decoder downstream has no way to tell which it was given. Without
# `error` a controller's bus-off report looks like traffic from a device that
# does not exist.
struct CanFrame {
  # 11 bits when `extended` is false, 29 bits when it is true.
  id @0 :UInt32;

  # Payload length in bytes. Only the first `len` bytes of `data` are valid.
  #
  # A real byte count, not a CAN FD length code: FD's DLC 9..15 mean 12, 16, 20,
  # 24, 32, 48 and 64 bytes, and that translation belongs in the driver rather
  # than in everything that reads this.
  len @1 :UInt8;

  # Up to 8 bytes for classic CAN, up to 64 for CAN FD.
  data @2 :List(UInt8);

  # A 29-bit identifier rather than an 11-bit one.
  extended @3 :Bool;

  # Remote transmission request: a request for someone else to send this
  # identifier. Carries no payload, and does not exist in CAN FD.
  rtr @4 :Bool;

  # A CAN FD frame: up to 64 bytes, and no remote frames.
  fd @5 :Bool;

  # The FD data phase ran at the faster bit rate.
  brs @6 :Bool;

  # FD error state indicator: the transmitter was error-passive when it sent.
  esi @7 :Bool;

  # Not a message -- the controller reporting a bus condition. `id` carries a
  # backend-specific bitmap of what went wrong rather than an address, so treat
  # this as a signal to look at the bus status rather than something to decode.
  error @8 :Bool;

  # When the frame was seen, in microseconds. Comes from the adapter or the
  # kernel where either supplies one, which is far closer to the wire than a
  # time taken after USB and scheduling latency. Zero means no timestamp.
  timestampUs @9 :UInt64;

  # Which channel this came from, for a consumer merging several. Empty when
  # the publisher only has one.
  channel @10 :Text;
}
