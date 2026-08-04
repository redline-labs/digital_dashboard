@0xb1a7c0ffee5eed01;

# Fixtures for schemas_test_registry_plugin. Every declaration here is a shape
# the CMake regex that used to build the registry got wrong. This directory is
# deliberately not covered by the *.capnp glob in ../CMakeLists.txt -- these
# are inputs to a test, not schemas on the bus.

# The regex was "struct[ \t]+([a-zA-Z_][a-zA-Z0-9_]*)[ \t]", which needs
# whitespace after the name. With the brace hard against it there was no match,
# so this struct was silently absent from the registry: it compiled, it
# generated, and nothing failed until a subscriber could not decode the topic.
struct Tight{
  value @0 :UInt32;
}

# The regex never stripped comments, so the word "struct Ghost" in this
# sentence was extracted as a type and the generated code failed to compile
# against a type that does not exist.
struct Spacious {
  value @0 :UInt32;
}

# Nested structs were matched by their unqualified name, so the generated
# capnp::Schema::from<Inner>() did not compile. Schemas in this tree were
# written flat to work around it.
struct Outer {
  value @0 :UInt32;

  struct Inner {
    value @0 :UInt32;
  }

  # Nested enums were always fine -- the regex only looked for "struct" -- but
  # they must still not become registry entries: an enum cannot be the root of
  # a message, so it cannot be published.
  enum Mode {
    idle @0;
    busy @1;
  }
}

# A group is reached through a field rather than through nestedNodes. It is not
# a standalone type and capnp::Schema::from<> would not compile for one.
struct WithGroup {
  pair :group {
    first @0 :UInt32;
    second @1 :UInt32;
  }
}
