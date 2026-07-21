@0xddc7506223751f8f;

# Coarse CarPlay driver state, published on change and periodically.
struct CarPlaySessionState {
  deviceConnected @0 :Bool;
  phase           @1 :Phase;
  nightMode       @2 :Bool;
  mainWidthPx     @3 :UInt16;
  mainHeightPx    @4 :UInt16;
  deviceName      @5 :Text;
  # Set while the phone wants microphone audio (Siri or an active call).
  # The widget starts/stops capture on nodes/carplay/mic in response.
  micActive       @6 :Bool;
  micSampleRateHz @7 :UInt32;
  micChannels     @8 :UInt8;

  enum Phase {
    idle             @0;
    usbConfig        @1;
    lockdown         @2;
    iap2             @3;
    ncmUp            @4;
    airplayHandshake @5;
    recording        @6;
    error            @7;
  }
}
