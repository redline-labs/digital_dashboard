@0xaf9018b7ccbf329f;

# Phone call state from iAP2 CallStateUpdate.
struct CarPlayCall {
  state        @0 :State;
  remoteName   @1 :Text;
  remoteNumber @2 :Text;
  durationSec  @3 :Float32;

  enum State {
    idle         @0;
    incoming     @1;
    dialing      @2;
    active       @3;
    held         @4;
    disconnected @5;
  }
}
