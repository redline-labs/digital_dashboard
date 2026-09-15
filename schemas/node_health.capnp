@0xe841aa6a037bcf01;

# How a node says it is working, in one shape every node shares.
#
# Published on `nodes/<name>/health` by node_health::HealthReporter: once per
# `periodMs` as a heartbeat, and again at once when any state changes. Detailed
# device status stays in each node's own schema (CanBridgeStatus, Bd992Status,
# ...); this is the summary a monitor can read without knowing any of them.
#
# A heartbeat is what tells "working" from "hung". The liveliness token behind
# pub_sub::NodeIdentity stays up for a process whose main loop is stuck, so a
# monitor uses both: identity gone means the process went away, a heartbeat
# older than a few periods means it is still there and not doing its job.
#
# Fields are only ever appended. A monitor built against an older copy of this
# schema must still read a newer node's samples.

enum HealthState {
  # Never set; also what a monitor maps an enumerant it does not know to.
  unknown @0;
  # Running, not yet serving: a device still opening, a graph still loading.
  starting @1;
  ok @2;
  # Working, with something wrong a person should look at.
  degraded @3;
  # Not doing its job.
  fault @4;
  # Shutting down on purpose. The last sample a clean exit sends.
  stopping @5;
}

struct HealthCheck {
  # Stable, short, and unique within the node: `can_rx`, `channel:can0`.
  name @0 :Text;
  state @1 :HealthState;
  # Why, when the state is not ok. Empty when there is nothing to add.
  detail @2 :Text;
  # How long this check has held its current state.
  stateAgeMs @3 :UInt64;
}

struct NodeHealth {
  # The name the node announces through pub_sub::NodeIdentity.
  node @0 :Text;
  # The publishing session's id, so a monitor can join this against the node
  # directory without depending on the sample's timestamp.
  zid @1 :Text;
  # The worst of the checks, or starting / stopping around the node's lifetime.
  state @2 :HealthState;
  # Counts up by one per sample. A gap means samples were lost; going
  # backwards means the node restarted.
  sequence @3 :UInt64;
  uptimeMs @4 :UInt64;
  # The heartbeat period this node promises. A monitor calls a node late when
  # several periods pass with no sample.
  periodMs @5 :UInt32;
  pid @6 :UInt32;
  checks @7 :List(HealthCheck);
}
