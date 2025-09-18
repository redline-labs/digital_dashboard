@0x9cf0e7f8b3b8a7b1;

struct CanFrame {
  id @0 :UInt32;
  # Only first 'len' bytes are valid in data
  len @1 :UInt8;
  data @2 :List(UInt8);  # Expect up to 8 bytes
}


