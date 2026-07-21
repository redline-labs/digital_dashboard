@0x897e97c464abf6a5;

# One H.264/H.265 Annex-B access unit from the CarPlay driver node.
# isConfig marks SPS/PPS(/VPS) parameter sets (sent on stream start and
# whenever the phone renegotiates); a restarted subscriber should discard
# frames until it has seen a config followed by a keyframe.
struct CarPlayVideo {
  seq        @0 :UInt32;
  codec      @1 :Codec;
  isConfig   @2 :Bool;
  isKeyframe @3 :Bool;
  widthPx    @4 :UInt16;
  heightPx   @5 :UInt16;
  ptsUsec    @6 :UInt64;
  data       @7 :Data;

  enum Codec {
    h264 @0;
    h265 @1;
  }
}
