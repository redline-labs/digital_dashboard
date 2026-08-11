@0xdfc5a0b8b4a87a1b;

# MSEL Solid State Master Relay: what the isolator reports, and what it can be
# told.
#
# The enums below all carry an explicit `unknown` at ordinal 0. A capnp enum
# field cannot be absent, and the device is entitled to report a value its
# manual does not document -- a firmware newer than this schema, or a unit
# configured by something other than us. Coercing that onto whichever neighbour
# happens to be adjacent would put a confident, wrong setting in front of
# someone about to change it, so it decodes as `unknown` and the matching `*Raw`
# field carries the byte that actually arrived.
#
# Every settings change also travels with two facts the caller cannot work out
# from the request: whether the relay has to be power-cycled before the change
# is live, and that a human has to be holding the external kill switch on the
# device for the command to be accepted at all. See MselCommandResponse.

enum MselRelayStatus {
  # Ordinals match the device's own status enumeration, so 0 doubles as "no
  # shutdown recorded yet" in the two shutdown-cause fields, which is exactly
  # what the relay sends there before it has ever shut down.
  unknown @0;
  normal @1;
  overTemperatureWarning @2;
  overCurrentWarning @3;
  lowVoltageWarning @4;
  highVoltageWarning @5;
  overTemperatureKill @6;
  driverSwitchKill @7;
  externalSwitchKill @8;
  canTriggerKill @9;
  powerOnReset @10;
}

enum MselCanBaud {
  unknown @0;
  rate1Mbps @1;
  rate500Kbps @2;
  rate250Kbps @3;
}

enum MselOutputDrive {
  unknown @0;
  activeHighHalfBridge @1;
  activeHighHighSide @2;
  activeHighLowSide @3;
  activeLowHalfBridge @4;
  activeLowHighSide @5;
  activeLowLowSide @6;
}

enum MselCanKillMode {
  unknown @0;
  disabled @1;
  enabled @2;
  # Listens for a MoTeC Accident Data Recorder's severe-event message rather
  # than a plain kill frame.
  accidentDataRecorder @3;
}

enum MselSwitchState {
  unknown @0;
  normalCalNormalExternalSwitch @1;
  normalCalInternalExternalSwitch @2;
  legacyCalNormalExternalSwitch @3;
  legacyCalInternalExternalSwitch @4;
}

# Request-only, so there is no undocumented value to represent: these are the
# only two rates the device accepts.
enum MselTransmitRate {
  hz10 @0;
  hz100 @1;
}

enum MselConfigResponse {
  # ORDINAL ZERO ON PURPOSE, and it is not one of the device's codes. This is
  # the value the field has when nothing has been written into it, which is
  # every command the relay did not answer -- and silence is the ordinary
  # outcome here, not a rare one. With `success` at zero, as it was, an
  # unanswered command reported "success" beside `ok: false`, which is the one
  # combination a reader is most likely to misread in the one direction that
  # matters.
  noAnswer @0;

  success @1;
  idMismatch @2;
  frameCheckError @3;
  invalidId @4;
}

struct MselMasterRelayStatus {
    status @0 : UInt8;                       # raw status byte
    overTempWarn @1 : Bool;
    externalKill @2 : Bool;
    driverKill @3 : Bool;
    overTempKill @4 : Bool;
    highVoltageWarn @5 : Bool;
    lowVoltageWarn @6 : Bool;
    overCurrentWarn @7 : Bool;
    canKill @8 : Bool;
    temperatureInternal @9 : Float32;        # deg C
    loadCurrent @10 : Float32;               # A, positive when discharging
    voltageOut @11 : Float32;                # V

    # The same status byte, named. `unknown` means the device reported a value
    # this schema does not know, and `status` above still has the raw byte.
    state @12 : MselRelayStatus;

    # The eight booleans above as the bitmask the device actually sends. Not
    # redundant in practice: it is the compact form for a dashboard widget or a
    # log line, and it is what the manual's troubleshooting section quotes.
    warnings @13 : UInt8;
}

struct MselMasterRelayInfo {
    shutdownCause2 @0 : UInt8;
    shutdownCause @1 : UInt8;
    timeSinceShutdown @2 : Float32;          # seconds
    configShutdownDelay @3 : Float32;        # seconds
    configCanKill @4 : UInt8;                # enum-like
    configCanBaud @5 : UInt8;                # enum-like
    configOutputDrive @6 : UInt8;            # enum-like
    serialNo @7 : UInt32;
    voltageIn @8 : Float32;                  # V

    # The two shutdown causes, named. Both are `unknown` on a relay that has
    # never shut down.
    shutdownCauseState @9 : MselRelayStatus;
    shutdownCause2State @10 : MselRelayStatus;
}

# Only transmitted by units at serial number 64666 or above. A relay that never
# publishes this is old, not broken.
struct MselMasterRelaySwitchState {
    switchState @0 : MselSwitchState;
    switchStateRaw @1 : UInt8;
}

# The relay's stored settings, read back from its own info message.
#
# Published as a topic rather than only being available through the read-back
# service, so that current settings can be watched with `inspect echo` and
# recorded into a bag alongside everything else -- a setting that changed
# mid-session is otherwise invisible after the fact.
struct MselMasterRelayConfig {
    canKill @0 : MselCanKillMode;
    canKillRaw @1 : UInt8;
    baud @2 : MselCanBaud;
    baudRaw @3 : UInt8;
    outputDrive @4 : MselOutputDrive;
    outputDriveRaw @5 : UInt8;

    # Held-up time after a shutdown event. Always a whole multiple of 100ms:
    # the device stores tenths of a second.
    shutdownDelayMs @6 : UInt16;
}

# There is no separate response message. The relay's answer to a configuration
# command is waited for by the node and returned in MselCommandResponse, so a
# caller learns what happened from the call it made rather than by correlating
# a service reply with a topic it also had to be subscribed to.

# Read the current settings. Takes no arguments; call it with `--data '{}'`.
struct MselGetSettingsRequest {
}

struct MselGetSettingsResponse {
    ok @0 : Bool;
    error @1 : Text;

    # False until the relay's info message has been seen at least once. A relay
    # that is unpowered, on a different base address, or on a bus running at a
    # different rate will leave this false, and `config` meaningless -- which is
    # the difference between "the shutdown delay is zero" and "we do not know
    # what the shutdown delay is".
    valid @2 : Bool;

    config @3 : MselMasterRelayConfig;
    serialNo @4 : UInt32;

    # What this node is decoding at, and where it would send a kill frame. Not
    # read from the device -- the relay does not report its own base address --
    # but from the node's configuration, so it answers "why am I seeing
    # nothing".
    baseAddress @5 : UInt16;
    killAddress @6 : UInt16;

    # Whether this node was started with remote shutdown permitted. Independent
    # of whether the relay itself has it enabled, which is `config.canKill`.
    canKillAllowed @7 : Bool;
}

# Shared by every command. The three flags after `error` are the operating
# instructions for the change that was just made, returned as data rather than
# written to a log the caller cannot see.
struct MselCommandResponse {
    # THE RELAY ACCEPTED IT. Not "the frame was sent" -- the node waits for the
    # relay's acknowledgement and reports what it said, so a caller that gets
    # `ok` can act on it. False covers all three ways this fails: the command
    # was refused here, it went out and was never answered, or it was answered
    # with a rejection. `answered` and `error` say which.
    ok @0 : Bool;

    # Empty when ok. Otherwise why not, in the words the caller needs: a value
    # the device cannot store, a command this node was not permitted to send,
    # the relay's rejection code, or the timeout and what usually causes it.
    error @1 : Text;

    # The exact bytes that went onto the bus, as hex. Empty when nothing was
    # sent, which is how a locally refused command is told apart from one the
    # relay ignored.
    frameSent @2 : Text;

    # True when the relay has to be power-cycled before this change is live.
    # Everything except the base address needs it.
    requiresPowerCycle @3 : Bool;

    # True whenever a command was sent, because every configuration command
    # needs the external kill switch pressed and held while it is transmitted.
    # There is no software substitute; without it the relay ignores the frame
    # and never answers.
    holdExternalKillSwitch @4 : Bool;

    # Whether the relay answered at all within the node's wait window.
    #
    # FALSE IS THE ORDINARY OUTCOME, not a fault: an unheld external kill switch
    # produces silence rather than a rejection, and so does a relay that is
    # unpowered, re-addressed or on a bus at a different rate. It is the one
    # field that separates "the relay said no" from "nothing answered", and the
    # two need completely different things done about them.
    answered @5 : Bool;

    # What it said. Meaningless unless `answered`.
    response @6 : MselConfigResponse;
    responseRaw @7 : UInt8;

    # How long the node waited before giving up or being answered, in
    # milliseconds. Reported so a caller can tell a prompt refusal from one that
    # sat out the whole window, and so the configured timeout is visible without
    # reading the node's config.
    waitedMs @8 : UInt16;

    # The kill trigger is not acknowledged by the relay at all -- it is acted on
    # the moment it arrives, which is the point of it. True says this node did
    # not wait for an answer and that `answered: false` means nothing here.
    notAcknowledged @9 : Bool;
}

# Moves all three periodic messages. The node retunes its decoder to match, so
# telemetry keeps flowing without a restart -- but nothing else on the bus
# knows, and neither does a logger reading the old identifiers.
struct MselSetBaseAddressRequest {
    baseAddress @0 : UInt16;
}

struct MselSetTransmitRateRequest {
    rate @0 : MselTransmitRate;
}

# Bus rate and shutdown delay travel in one command because the device packs
# them into one frame.
#
# Changing the baud rate is the most dangerous thing here: the relay comes back
# at the new rate after a power cycle, and if the rest of the bus does not move
# with it the relay simply disappears. The delay half takes effect immediately;
# the baud half does not.
struct MselSetBaudAndShutdownDelayRequest {
    baud @0 : MselCanBaud;

    # Must be a whole multiple of 100ms and no more than 25500ms. Values in
    # between are rejected rather than rounded, so that asking for 150ms does
    # not silently store 100ms.
    shutdownDelayMs @1 : UInt16;
}

struct MselSetOutputDriveRequest {
    drive @0 : MselOutputDrive;
}

struct MselSetCanShutdownRequest {
    mode @0 : MselCanKillMode;

    # Where the relay will listen for a kill frame. Ignored by the device when
    # mode is `disabled`, but still validated here.
    killAddress @1 : UInt16;
}

# Isolates the battery and shuts the engine down, remotely, over CAN.
#
# Refused unless the node was started with this explicitly permitted AND this
# field carries the exact confirmation token. Two independent gates, because
# neither a stray service call nor a config file left in a permissive state
# should be able to stop a moving vehicle on its own.
struct MselCanKillRequest {
    confirm @0 : Text;
}
