@0xc6b7ea85adda77fb;

# The node's own topics and services: what it is doing, and how to ask it to do
# something else.

# A GSOF record this build does not model, passed through as bytes.
#
# Published rather than dropped because a receiver enabled for a record we have
# never heard of is otherwise indistinguishable from one that is silent, and
# because the bytes are enough to decode offline and enough to justify adding a
# row to GSOF_RECORD_TABLE.
struct GsofRawRecord {
  recordType @0 :UInt8;
  bytes @1 :Data;
}

# One output the receiver is configured to emit, or that the node wants it to.
struct Bd992OutputMessage {
  # Zero-based, as the wire has it. 20, 21 and 22 are the first three IP
  # sockets; the ICD's prose numbers them from one, which is a good way to be
  # off by one.
  portIndex @0 :UInt8;

  outputType @1 :UInt8;
  outputTypeName @2 :Text;

  # The frequency byte, and its meaning. The byte values are NOT ordinals --
  # 0x01 is 10 Hz and 0x03 is 1 Hz -- so the name is what to read.
  rate @3 :UInt8;
  rateName @4 :Text;

  offsetSeconds @5 :UInt8;

  # Set when outputType is GSOF, in which case gsofRecordType names which
  # record and gsofRecordName is its name from GSOF_RECORD_TABLE.
  isGsof @6 :Bool;
  gsofRecordType @7 :UInt8;
  gsofRecordName @8 :Text;
}

enum Bd992ChangeKind {
  unknown @0;
  # The configuration asks for an output the receiver does not have, or has
  # turned off.
  missing @1;
  # The receiver has the output at a different rate.
  rateDrift @2;
  # The receiver has an output the configuration does not mention. Only acted
  # on under the exclusive port policy.
  unexpected @3;
}

struct Bd992ConfigChange {
  kind @0 :Bd992ChangeKind;
  # A one-line description, already formatted -- the same text the node logs.
  description @1 :Text;
  desired @2 :Bd992OutputMessage;
  actual @3 :Bd992OutputMessage;
}

enum Bd992ConfigMode {
  unknown @0;
  # Read the receiver's configuration, report drift, change nothing.
  reportOnly @1;
  # Read first, then write only what drifted.
  enforce @2;
}

# What the node is doing. Published on a timer and on a change, so a late
# subscriber does not have to wait for something to go wrong.
struct Bd992Status {
  # --- the two connections ---
  streamConnected @0 :Bool;
  controlConnected @1 :Bool;
  receiverHost @2 :Text;
  streamPort @3 :UInt16;
  controlPort @4 :UInt16;

  streamConnects @5 :UInt64;
  streamConnectFailures @6 :UInt64;
  streamDisconnects @7 :UInt64;

  # Empty unless something failed. The reason the last connection attempt did
  # not work, which is the first thing to look at when no topics appear.
  lastError @8 :Text;

  # --- the byte pipeline ---
  bytesRead @9 :UInt64;
  packets @10 :UInt64;

  # Packets whose framing was right and whose checksum was not. Non-zero with a
  # healthy packet count is a noisy link.
  checksumErrors @11 :UInt64;
  framingErrors @12 :UInt64;

  # How many times the framer had to hunt for a new packet boundary, and how
  # many bytes that cost. One resync that discarded 40 kB is a different
  # problem from 400 that discarded one byte each.
  resyncs @13 :UInt64;
  droppedBytes @14 :UInt64;

  # --- reassembly and records ---
  transmissions @15 :UInt64;
  # Pages buffered towards a transmission that was then abandoned. Non-zero
  # means pages are being lost.
  pagesDiscarded @16 :UInt64;

  records @17 :UInt64;
  # Records whose type is not in GSOF_RECORD_TABLE. A configuration choice, not
  # a fault: published on the raw topic.
  unknownRecords @18 :UInt64;
  # Records whose type IS known but whose body did not decode. A bug or a
  # firmware change.
  malformedRecords @19 :UInt64;

  # --- per record type ---
  seen @20 :List(Bd992RecordSeen);

  # --- configuration ---
  configMode @21 :Bd992ConfigMode;
  portPolicy @22 :Text;
  configuredPortIndex @23 :UInt8;

  # Whether the last comparison found the receiver configured as asked.
  configChecked @24 :Bool;
  configMatches @25 :Bool;
  # What did not match, at the last check.
  drift @26 :List(Bd992ConfigChange);
  # Outputs the node actually wrote since it started. Should be zero on a
  # healthy system that nobody has reconfigured; a climbing value means
  # something else keeps changing the receiver back.
  outputsCorrected @27 :UInt64;
}

# How long ago each record type was last seen.
#
# THE field that distinguishes "the receiver is fine" from "the receiver
# quietly stopped sending record 27". A topic that has gone silent looks
# identical to one that was never enabled unless something reports the age.
struct Bd992RecordSeen {
  recordType @0 :UInt8;
  recordName @1 :Text;
  count @2 :UInt64;
  ageMs @3 :UInt64;
}

# ============================================================================
# Services
# ============================================================================

# Read the receiver's current output configuration. Backed by GETAPPFILE (65h).
struct Bd992GetOutputConfigRequest {
  # Which stored application file. Zero asks the node to use the index from its
  # own configuration, which is the usual case -- the ICD documents index 0 as
  # the factory defaults and does not say which one is running.
  applicationFileIndex @0 :UInt16;
}

struct Bd992GetOutputConfigResponse {
  ok @0 :Bool;
  error @1 :Text;

  outputs @2 :List(Bd992OutputMessage);

  # Application file records that are not output messages -- antenna,
  # reference station. Counted, not decoded: the node has no business
  # rewriting them, but "there are six settings we did not touch" is worth
  # knowing.
  otherRecordCount @3 :UInt16;

  # What the receiver calls itself. Echoed back on every write rather than
  # guessed, because the ICD publishes no device type values.
  deviceType @4 :UInt8;
}

# Change the output configuration. Backed by APPFILE (64h), after a read.
struct Bd992SetOutputConfigRequest {
  # The outputs wanted on the port. Only portIndex, rate and gsofRecordType are
  # read; the name fields are ignored on the way in.
  outputs @0 :List(Bd992OutputMessage);

  # Which port the list describes. Outputs on any other port are untouched,
  # and are not reported.
  portIndex @1 :UInt8;

  # "additive" leaves outputs not in the list alone; "exclusive" turns them
  # off. Empty uses the node's configured policy.
  portPolicy @2 :Text;

  # Work out what would change and report it WITHOUT writing anything. The
  # safe way to find out what a configuration change would do.
  dryRun @3 :Bool;
}

struct Bd992SetOutputConfigResponse {
  ok @0 :Bool;
  error @1 :Text;

  # What differed from what the receiver already had. Empty means the receiver
  # was already configured as asked and nothing was sent.
  changes @2 :List(Bd992ConfigChange);

  # False when dryRun was set, or when there was nothing to do.
  written @3 :Bool;
}

# Re-run the configuration pass from the node's own YAML, now.
struct Bd992ApplyConfigRequest {
  # Report the difference without writing, whatever mode the node is in.
  # Cannot be used the other way round: a node in report-only mode will not
  # write because a service asked it to.
  dryRun @0 :Bool;
}

struct Bd992ApplyConfigResponse {
  ok @0 :Bool;
  error @1 :Text;
  mode @2 :Bd992ConfigMode;
  changes @3 :List(Bd992ConfigChange);
  written @4 :Bool;
}

# Receiver identity and installed options. Backed by GETOPT (4Ah) and by
# whatever GSOF 15 last reported.
struct Bd992GetReceiverInfoRequest {
  optionsPage @0 :UInt8;
}

struct Bd992GetReceiverInfoResponse {
  ok @0 :Bool;
  error @1 :Text;

  # From GSOF 15, if it has been seen. Zero otherwise.
  serialNumber @2 :Int32;
  serialNumberKnown @3 :Bool;

  deviceType @4 :UInt8;

  # The RETOPT payload, undecoded. The option list's layout is not modelled,
  # and the bytes are more useful than nothing.
  optionsRaw @5 :Data;
}

# The escape hatch: any packet type, any payload.
#
# This is what stops every future ICD packet being a code change. It is off
# unless allow_raw_commands is set in the node's config, because an arbitrary
# command can leave a receiver unreachable.
struct Bd992SendCommandRequest {
  packetType @0 :UInt8;
  data @1 :Data;
}

struct Bd992SendCommandResponse {
  ok @0 :Bool;
  error @1 :Text;

  replyStatus @2 :UInt8;
  replyType @3 :UInt8;
  replyData @4 :Data;
}
