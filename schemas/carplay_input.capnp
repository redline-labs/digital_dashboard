@0x9e75ac777970d2cb;

# User input from a UI widget to the CarPlay driver node, which converts it
# to HID reports for the phone. Touch coordinates are normalized to 0..10000
# over the widget area; the driver rescales to the phone's 0..1 range.
struct CarPlayInput {
  kind  @0 :Kind;
  x     @1 :UInt16;
  y     @2 :UInt16;
  code  @3 :UInt16;
  value @4 :Int32;

  enum Kind {
    touchDown @0;
    touchMove @1;
    touchUp   @2;
    knob      @3;
    mediaKey  @4;
    siri      @5;
    telephony @6;
  }
}
