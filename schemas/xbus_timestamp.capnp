@0xda0463e832d84f6d;

using Common = import "xbus_common.capnp";

# XDI 0x1010. The device's own UTC clock.
#
# ON AN MTi-610 THERE IS NO GNSS BEHIND THIS. On a 670 this field is
# disciplined by the receiver; on a 610 it is a free-running clock that is only
# as good as the last SetUtcTime a host sent it, and this node sends none. The
# three validity flags are the device's own claim about it, and 
# `dateValid` being false on a device nobody has set is the expected state,
# not a fault.
struct XbusUtcTime {
  header @0 :Common.XbusSampleHeader;

  year @1 :UInt16;
  month @2 :UInt8;
  day @3 :UInt8;
  hour @4 :UInt8;
  minute @5 :UInt8;
  second @6 :UInt8;
  nanosecond @7 :UInt32;

  # The flags byte, raw and decoded, for the reason the status word is: Xsens
  # reserves the upper bits.
  flags @8 :UInt8;
  dateValid @9 :Bool;
  timeValid @10 :Bool;
  fullyResolved @11 :Bool;
}
