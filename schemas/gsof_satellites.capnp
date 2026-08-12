@0xdfad6f652169e0cc;

using Common = import "gsof_common.capnp";

# The satellites in view, briefly (GSOF 33) and in detail (GSOF 34).
#
# Both lists are bounded by the record's one-byte length field: (255-1)/4 for
# the brief form and (255-1)/10 for the detailed one. Those are the caps the
# library enforces, and they are why neither list is annotated with a fixed
# length here -- the count varies with the sky, not with the configuration.

struct GsofAllSvBrief {
  count @0 :UInt8;
  satellites @1 :List(Common.GsofSvBrief);
}

struct GsofAllSvDetailed {
  count @0 :UInt8;
  satellites @1 :List(Common.GsofSvDetail);
}
