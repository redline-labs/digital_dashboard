@0xf417b2c22d200457;

# Schema-level facts that Cap'n Proto's type system cannot express, but which
# every consumer of a schema needs.
#
# Annotations are compile-time metadata. They change NOTHING about the wire
# format -- a message encoded before an annotation was added decodes identically
# afterwards -- and they travel inside the schema node, so anything holding a
# capnp::Schema can read them back at runtime without a side table to keep in
# sync.

# How many elements a List field always carries.
#
# WHY THIS HAS TO EXIST. A capnp list declares no length: `List(Float32)` is any
# number of floats, and the count is a property of each MESSAGE. That is right
# for a genuinely variable list and wrong for the ones this bus actually carries
# -- a MoTeC PDM has thirty-two outputs, always, and "how many outputs are
# there" is a fact about the device, not about the last packet that happened to
# arrive.
#
# Without it, everything downstream has to guess or wait:
#
#   - A picker cannot offer the individual elements as bindable channels. It
#     either guesses a count and offers rows that do not exist -- which bind and
#     then produce nothing, looking exactly like a dead publisher -- or it peeks
#     at a live message, which means a browser that shows different things
#     depending on whether traffic happened to be flowing.
#
#   - An expression indexing past the end cannot be rejected until a message
#     arrives. `values[40]` on a thirty-two output PDM has to compile, bind, and
#     only then start dropping samples. With the length declared it is a
#     construction error, which is where this tree prefers to fail.
#
# Applies to the FIELD, not to the type, because the same List(Float32) means
# thirty-two outputs on one field and twenty-three inputs on another.
#
# A publisher that sends a different count is not silently tolerated: the
# annotation is a claim about the schema, and a message that contradicts it is a
# publisher bug worth reporting rather than working around.
annotation fixedLength @0x896cba7e8df9da6e (field) :UInt32;
