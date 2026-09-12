@0xc57ef72791ebadc6;

# The node's own messages: its status, the items it could not model, and the
# request/response pairs behind its services.

# An MTData2 item whose data identifier is not in XBUS_DATA_TABLE.
#
# Published rather than dropped, for the reason nodes/bd992_bridge publishes
# unmodelled GSOF records: a device emitting something unrecognised is
# otherwise indistinguishable from one that is silent. On an MTi-610 a nonzero
# rate here means the device is NOT a 610 -- a 620, 630 or 670 has outputs this
# build does not model, and this topic is where they appear.
struct XbusRawItem {
  # The identifier exactly as it arrived, format nibble included.
  rawDataId @0 :UInt16;

  # The group, for a log line that can say "this is an orientation output"
  # without a lookup table to hand.
  groupName @1 :Text;

  payload @2 :Data;
}

# One row of the device's output configuration.
struct Mti610OutputEntry {
  rawDataId @0 :UInt16;

  # The name from XBUS_DATA_TABLE, or "unknown" for an identifier this build
  # does not model.
  name @1 :Text;

  # 65535 means "as fast as the device can", and is also what the device
  # reports for anything that accompanies every packet whatever was asked for.
  frequencyHz @2 :UInt16;
}

enum Mti610ConfigMode {
  unknown @0;
  enforce @1;
  reportOnly @2;
}

enum Mti610ChangeKind {
  unknown @0;
  missing @1;
  rateDrift @2;
  unexpected @3;
}

# One difference between what the device reports and what the config asks for.
struct Mti610ConfigChange {
  kind @0 :Mti610ChangeKind;
  rawDataId @1 :UInt16;
  name @2 :Text;
  actualHz @3 :UInt16;
  desiredHz @4 :UInt16;
}

# What the node is doing, once a second.
struct Mti610Status {
  # False between losing the port and reopening it. The first thing to look at.
  connected @0 :Bool;

  # True once the handshake has completed and the device is in Measurement
  # state. A device can be connected and not measuring -- that is what a failed
  # configuration looks like.
  measuring @1 :Bool;

  port @2 :Text;
  baud @3 :UInt32;

  deviceId @4 :UInt64;
  productCode @5 :Text;
  firmwareVersion @6 :Text;
  hardwareVersion @7 :Text;

  # False when the product code says this is not the device this build models.
  # Not an error -- the node talks to it anyway and its extra outputs land on
  # the raw topic -- but it explains a raw topic that is unexpectedly busy.
  isMti610 @8 :Bool;

  opens @9 :UInt64;
  bytesRead @10 :UInt64;
  dataMessages @11 :UInt64;
  items @12 :UInt64;

  # Items whose identifier is not modelled. Distinct from malformedItems: this
  # is a device sending something we do not know, that one is something we
  # claim to know and got wrong. They look identical in a plot and mean
  # opposite things.
  unknownItems @13 :UInt64;
  malformedItems @14 :UInt64;
  truncatedBodies @15 :UInt64;

  # Times the device reset out from under the node. Nonzero means power.
  deviceResets @16 :UInt64;

  # Framer health. A checksum error count that climbs with a healthy message
  # count is a noisy link; one that climbs with NO messages is the wrong baud
  # rate.
  framedMessages @17 :UInt64;
  checksumErrors @18 :UInt64;
  resyncs @19 :UInt64;
  droppedBytes @20 :UInt64;
  bufferOverflows @21 :UInt64;

  configMode @22 :Mti610ConfigMode;
  configChecks @23 :UInt64;
  configWrites @24 :UInt64;

  # What the device says it is actually sending, which is not necessarily what
  # was asked for: it clamps a rate it cannot sustain rather than refusing.
  effectiveOutputs @25 :List(Mti610OutputEntry);

  # What the last comparison found. Empty means the device matches.
  changes @26 :List(Mti610ConfigChange);

  lastError @27 :Text;

  # Which data identifiers have been seen at least once, and how many of each.
  seen @28 :List(Mti610SeenItem);
}

struct Mti610SeenItem {
  rawDataId @0 :UInt16;
  name @1 :Text;
  count @2 :UInt64;
}

# --- services -------------------------------------------------------------

struct Mti610GetOutputConfigRequest {
  # Read the configuration from the device rather than reporting the cached
  # one. Costs a trip through Config state, which STOPS THE DATA for as long as
  # it takes -- so it is opt-in rather than the default.
  refresh @0 :Bool;
}

struct Mti610GetOutputConfigResponse {
  ok @0 :Bool;
  error @1 :Text;
  entries @2 :List(Mti610OutputEntry);
  changes @3 :List(Mti610ConfigChange);
}

struct Mti610SetOutputConfigRequest {
  entries @0 :List(Mti610OutputEntry);

  # Drop anything not listed rather than carrying it through. Since
  # SetOutputConfiguration replaces the whole list, "additive" means the node
  # re-sends what it is leaving alone -- see libs/mti610/output_config.h.
  exclusive @1 :Bool;
}

struct Mti610SetOutputConfigResponse {
  ok @0 :Bool;
  error @1 :Text;

  # What the device settled on. Compare with the request: a rate the link
  # cannot carry comes back lower, with no error.
  effective @2 :List(Mti610OutputEntry);
}

struct Mti610GetDeviceInfoRequest {
  dummy @0 :Bool;
}

struct Mti610GetDeviceInfoResponse {
  ok @0 :Bool;
  error @1 :Text;
  deviceId @2 :UInt64;
  productCode @3 :Text;
  firmwareVersion @4 :Text;
  hardwareVersion @5 :Text;
  isMti610 @6 :Bool;
}

struct Mti610ApplyConfigRequest {
  dummy @0 :Bool;
}

struct Mti610ApplyConfigResponse {
  ok @0 :Bool;
  error @1 :Text;
  wrote @2 :Bool;
  changes @3 :List(Mti610ConfigChange);
  effective @4 :List(Mti610OutputEntry);
}
