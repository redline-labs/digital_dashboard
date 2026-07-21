@0xd90876577e9a6bba;

# Media metadata from iAP2 NowPlaying updates. albumArt (JPEG/PNG bytes,
# delivered via iAP2 file transfer) is only populated on track change and
# may be empty.
struct CarPlayNowPlaying {
  title       @0 :Text;
  artist      @1 :Text;
  album       @2 :Text;
  app         @3 :Text;
  durationSec @4 :Float32;
  elapsedSec  @5 :Float32;
  playing     @6 :Bool;
  albumArtSeq @7 :UInt32;
  albumArt    @8 :Data;
}
