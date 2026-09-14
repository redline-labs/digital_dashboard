@0xd1c3a5e7f9b2c4e6;

# Every shape pub_sub::jsonToCapnp and capnpToJson have to handle, in one
# struct. Parsed at test time with capnp::SchemaParser rather than compiled into
# the registry: nothing publishes this, and a registry entry would put a fake
# schema in front of every picker in the tree.

enum Colour {
  red @0;
  green @1;
  blue @2;
}

struct Inner {
  label @0 :Text;
  count @1 :UInt16;
}

struct Fixture {
  flag @0 :Bool;
  i8 @1 :Int8;
  u8 @2 :UInt8;
  i64 @3 :Int64;
  u64 @4 :UInt64;
  f32 @5 :Float32;
  f64 @6 :Float64;
  text @7 :Text;
  bytes @8 :Data;
  colour @9 :Colour;
  inner @10 :Inner;

  colours @11 :List(Colour);
  matrix @12 :List(List(UInt64));
  inners @13 :List(Inner);
  blobs @14 :List(Data);
  small @15 :List(UInt8);

  choice :union {
    none @16 :Void;
    speed @17 :Float32;
    name @18 :Text;
    nested @19 :Inner;
  }

  pair :group {
    a @20 :Int32;
    b @21 :Text;
  }
}
