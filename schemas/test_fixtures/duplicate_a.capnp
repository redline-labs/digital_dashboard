@0xb1a7c0ffee5eed02;

# Half of the duplicate-name pair. Compiled together with duplicate_b.capnp,
# these two must make the generator exit non-zero: a schema name is the identity
# of a message on the bus and in dashboard configs, so two of them cannot share
# one.
struct Collision {
  value @0 :UInt32;
}
