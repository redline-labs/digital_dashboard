@0xddc7506223751f8f;

# Coarse CarPlay driver state, published on change and periodically.
struct CarPlaySessionState {
  # True from the moment a phone is plugged in until it is unplugged, whether
  # or not CarPlay has started on it yet.
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

  # How far a phone has got. Declared in the order a bring-up passes through
  # them, so `phase` compares as progress up to `recording`.
  enum Phase {
    idle             @0;  # no phone
    usbConfig        @1;  # plugged in; switching it into its CarPlay USB configuration
    lockdown         @2;  # pairing -- waits on the phone's Trust prompt the first time
    ncmUp            @3;  # the USB network link to the phone is up
    iap2             @4;  # iAP2: authentication and identification
    airplayHandshake @5;  # the phone has been told where to connect; AirPlay is starting
    recording        @6;  # CarPlay is running and sending video
    error            @7;  # a bring-up failed with the phone still attached; retrying
  }
}
