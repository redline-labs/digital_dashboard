// The one translation unit that compiles mcap's implementation.
//
// mcap is header-only: writer.hpp and reader.hpp include writer.inl and
// reader.inl, guarded by MCAP_IMPLEMENTATION so that exactly one TU emits the
// definitions. Without this file every source that touches an mcap::Message
// would compile ~10,000 lines of implementation and the linker would then have
// to discard all but one copy.
//
// It must be the ONLY place MCAP_IMPLEMENTATION is defined. Defining it twice is
// a duplicate-symbol link error rather than anything subtle, which is the good
// case -- but it is worth saying here rather than leaving the next person to
// find out.

#define MCAP_IMPLEMENTATION
#include <mcap/reader.hpp>
#include <mcap/writer.hpp>
