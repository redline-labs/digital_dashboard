@0xacb8956ea3434953;

# Decoded PCM audio (s16le interleaved). Used in both directions:
# driver -> widget for downlink playback, widget -> driver for mic capture.
struct CarPlayAudio {
  sampleRateHz @0 :UInt32;
  channels     @1 :UInt8;
  streamType   @2 :StreamType;
  ptsUsec      @3 :UInt64;
  pcm          @4 :Data;

  enum StreamType {
    music     @0;
    navPrompt @1;
    siri      @2;
    call      @3;
    mic       @4;
  }
}
