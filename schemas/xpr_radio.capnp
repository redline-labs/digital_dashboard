@0xe3f6cc5f59622da2;

# A MOTOTRBO radio's state, and the two things worth asking it to do.
#
# The radio pushes its own state changes as unsolicited broadcasts, so the
# topics here are event-driven rather than polled: the channel topic is
# published when the radio says the channel changed, not once a second because
# something asked.

# Where the radio is, in its own terms.
#
# Zone and channel are the radio's indices, not names. Names live in the
# codeplug, which this build deliberately does not read -- see docs/xpr.md.
struct XprChannel {
  zone @0 :UInt16;
  channel @1 :UInt16;

  # How many zones the radio has, and how many channels are in the zone it is
  # currently on. The channel count is always for the CURRENT zone: the query's
  # argument is ignored by the radio, so there is no way to ask about another
  # one.
  zoneCount @2 :UInt16;
  channelsInZone @3 :UInt16;

  # True when this came from an unsolicited broadcast rather than from a query.
  # A broadcast is the radio telling us it moved; a query is us asking. They
  # can disagree for one message while a change is in flight.
  fromBroadcast @4 :Bool;
}

# The radio's own display, mirrored.
#
# The radio pushes one LINE at a time, so this message carries the whole screen
# as last seen rather than the line that changed -- a subscriber that joined
# between two lines would otherwise have half a screen and no way to know it.
struct XprDisplay {
  # Line 1 upwards, as the radio numbers them. Line 4 is the softkey row, whose
  # labels are separated by " | " once decoded.
  lines @0 :List(Text);

  # Which line the broadcast that produced this message carried.
  changedLine @1 :UInt8;
}

# A broadcast this build does not model, passed through as bytes.
#
# Published rather than dropped for the same reason bd992 publishes unknown
# GSOF records: a radio emitting something we have never seen is otherwise
# indistinguishable from a radio that has gone quiet. Five of the seven an XPR
# 5550 emits are here, and they stay here until a session that PROVOKES the
# state they report is captured -- three of them never changed value across a
# capture that included six channel changes, and a constant tells you nothing.
struct XprBroadcast {
  opcode @0 :UInt16;
  # "status", "event", "meter", "call-state", "capability", or "unknown".
  name @1 :Text;
  payload @2 :Data;
}

# What the radio calls itself. Filled in once, after the session comes up.
struct XprIdentity {
  modelNumber @0 :Text;
  serialNumber @1 :Text;
  firmwareVersion @2 :Text;

  # The kit/part number, e.g. "PMUE4140BK".
  tanapaNumber @3 :Text;

  # The radio's DMR id. It appears in no readable codeplug item under any
  # encoding, so the status query is the only way to learn it.
  radioId @4 :UInt32;
  radioIdKnown @5 :Bool;

  # Manufacture datecode, undecoded: seven bytes whose field layout is not
  # documented anywhere we have.
  datecode @6 :Data;
}

# What the node is doing. Published on a timer, so a late subscriber does not
# have to wait for something to go wrong.
struct XprRadioStatus {
  # --- the session ---
  connected @0 :Bool;
  radioHost @1 :Text;
  radioPort @2 :UInt16;
  # The XNL address the radio assigned us. Zero while disconnected.
  address @3 :UInt16;

  connects @4 :UInt64;
  connectFailures @5 :UInt64;
  disconnects @6 :UInt64;

  # Empty unless something failed. The first thing to look at when no topics
  # appear.
  lastError @7 :Text;

  # --- the message pipeline ---
  commands @8 :UInt64;
  replies @9 :UInt64;
  broadcasts @10 :UInt64;

  # Broadcasts thrown away because the node was not draining them fast enough.
  # Non-zero means the display and channel topics are behind the radio.
  broadcastsDropped @11 :UInt64;

  # Frames read while waiting for a reply that were neither the reply nor a
  # broadcast. A climbing count means queries are overlapping.
  framesSkipped @12 :UInt64;
  decodeErrors @13 :UInt64;

  # --- what it knows ---
  identity @14 :XprIdentity;
  identityKnown @15 :Bool;
  channel @16 :XprChannel;
  channelKnown @17 :Bool;

  # False when the node is configured read-only, in which case set_channel is
  # refused without touching the radio.
  channelControlEnabled @18 :Bool;
}

# ============================================================================
# Services
# ============================================================================

struct XprGetChannelRequest {
  # Ask the radio rather than answering from the last broadcast. Costs a round
  # trip; use it when the answer has to be current rather than recent.
  refresh @0 :Bool;
}

struct XprGetChannelResponse {
  ok @0 :Bool;
  error @1 :Text;
  channel @2 :XprChannel;
}

enum XprChannelOp {
  unknown @0;
  up @1;
  down @2;
  # Step until the radio reports the requested channel. There is no direct
  # select: the radio's own select operation returns success and does not move.
  select @3;
}

struct XprSetChannelRequest {
  op @0 :XprChannelOp;

  # For `select` only. The zone must be the one the radio is already on --
  # zone cannot be changed over this link at all, and a request for another
  # one is refused rather than approximated by stepping.
  zone @1 :UInt16;
  channel @2 :UInt16;
}

struct XprSetChannelResponse {
  ok @0 :Bool;
  error @1 :Text;
  # Where the radio ended up, whether or not that is where it was asked to go.
  channel @2 :XprChannel;
}

struct XprGetIdentityRequest {
  # Re-read from the radio instead of answering from what was read at connect.
  refresh @0 :Bool;
}

struct XprGetIdentityResponse {
  ok @0 :Bool;
  error @1 :Text;
  identity @2 :XprIdentity;
}
