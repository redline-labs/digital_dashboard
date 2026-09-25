@0x9e75ac777970d2cb;

# User input from a UI widget to the CarPlay driver node, which converts it
# to HID reports for the phone. Touch coordinates are normalized to 0..10000
# over the widget area; the driver rescales to the phone's 0..1 range.
#
# What `code` and `value` mean depends on `kind`:
#
#   touch                        every finger at once, in `contacts`; code
#                                and value unused. See below.
#   knob                         code is a KnobControl; value is the amount --
#                                detents for rotate (signed, positive
#                                clockwise), pixels for panX/panY, and 1/0 for
#                                the buttons. A button press with value 1 is a
#                                click: the driver releases it immediately, so
#                                a matching 0 is not required.
#   mediaKey                     code is a MediaKey; value unused.
#   telephony                    code is a TelephonyKey; value unused.
#   siri                         code and value unused -- one event asks the
#                                phone to start listening.
#
# WHY TOUCH TRAVELS AS A FRAME. The phone's digitizer report carries every
# contact slot in one report, so what it needs is the whole touch state at an
# instant. Sending one event per finger would make the driver reassemble that
# state, and two fingers moving together would reach the phone as two reports,
# the first with one finger moved and the other not -- a pinch that stutters.
# A frame is one Qt touch event, whole, and becomes exactly one HID report.
struct CarPlayInput {
  kind  @0 :Kind;
  code  @1 :UInt16;
  value @2 :Int32;

  contacts @3 :List(CarPlayTouchContact);
  # A touch event's fingers: one entry per finger on the glass, plus one with
  # down = false for each finger this frame lifts. A slot with no entry has no
  # finger on it. Empty for every other kind.

  enum Kind {
    touch     @0;
    knob      @1;
    mediaKey  @2;
    siri      @3;
    telephony @4;
  }

  # The controls on a rotary controller, as `code` for a knob event.
  enum KnobControl {
    select @0;
    home   @1;
    back   @2;
    rotate @3;
    panX   @4;
    panY   @5;
  }

  # `code` for a mediaKey event. The values are the usage indices in the HID
  # descriptor the driver advertises, so they are not free to renumber.
  enum MediaKey {
    none       @0;
    play       @1;
    pause      @2;
    playPause  @3;
    next       @4;
    previous   @5;
    navGuidance @6;
  }

  # `code` for a telephony event. Also HID usage indices. hookSwitch answers or
  # hangs up, drop ends the call, and key0..pound are DTMF digits.
  enum TelephonyKey {
    none       @0;
    hookSwitch @1;
    flash      @2;
    drop       @3;
    mute       @4;
    key0       @5;
    key1       @6;
    key2       @7;
    key3       @8;
    key4       @9;
    key5       @10;
    key6       @11;
    key7       @12;
    key8       @13;
    key9       @14;
    star       @15;
    pound      @16;
    delete     @17;
  }
}

# One finger in a touch event.
struct CarPlayTouchContact {
  slot @0 :UInt8;
  # Which of the phone's contact slots this finger occupies, 0 or 1. Fixed for
  # the finger's whole time on the glass: the phone tracks a finger by the slot
  # it keeps appearing in, so a finger that changed slot would read as one
  # lifting and another landing.

  x @1 :UInt16;
  y @2 :UInt16;
  # Normalized 0..10000 over the widget.

  down @3 :Bool;
  # False only in the frame that lifts this finger.
}
