---
title: protowire
parent: Libraries
---

# protowire

## Overview

Just enough protobuf for the two formats in this tree that are protobuf:
Mapbox Vector Tiles ([mvt](mvt.html)) and OpenStreetMap PBF ([osm](osm.html)).
Neither links a protobuf library. Between them the schemas are a few dozen
fields that have not changed in a decade, the encoding is four wire types of
which they use three, and pulling in libprotoc plus a generated-code step for
that would be a larger dependency than the formats it reads.

It is not a general protobuf implementation. It reads fields in the order they
appear, builds no message object, and knows nothing about required, optional or
default beyond what the caller applies. It was `libs/mvt`'s reader until
`libs/osm` needed it too, at which point leaving it there would have made an
OSM extractor depend on a vector-tile library; `mvt` re-exports every name into
namespace `mvt`, so nothing that used `mvt::Reader` changed. There is no spdlog
in it: errors come back through `Result<T>`, and whether one bad message in a
batch of tens of thousands is worth a log line is the caller's decision.

## Public headers

| Header | Declares |
|---|---|
| `protowire/reader.h` | `WireType`, `Field`, the cursor-style `Reader` (`field()`, `varint()`, `zigzag()`, `int32()`, `int64()`, `fixed32()`, `fixed64()`, `bytes()`, `text()`, `sub()`, `skip()`) and `constexpr unzigzag()`. |
| `protowire/error.h` | `Error` with kinds `Truncated`, `Malformed`, `Unsupported`, `Decompress`, a byte `offset`, `Result<T>` as `std::expected`, and the `truncated()`/`malformed()`/`unsupported()`/`decompress_failed()` constructors. |

## Using it

Link the `protowire` target. `tools/map_build` never links it directly; it
reaches it through `osm`, whose block decoder is the shape every caller takes.
A `Reader` borrows a span, and each field is read or skipped by its wire type.

```cpp
protowire::Reader reader(bytes);
while (!reader.done())
{
    auto field = reader.field();
    if (!field) return from_wire(field.error());
    switch (field->number)
    {
        case 16: { auto s = reader.text(); /* writingprogram */ break; }
        default: if (auto ok = reader.skip(field->wire); !ok) return from_wire(ok.error());
    }
}
```

`bytes()`, `text()` and `sub()` borrow from the buffer and stay valid only as
long as it does.

## Behaviour worth knowing

Every refusal in this library is a case where continuing would yield a
plausible number rather than a failure. A varint is at most ten bytes; a
continuation bit still set on the tenth is an error, because the alternative is
silent wrapping. Proto `int32` and `int64` are not zigzag: a negative value is
the ten-byte two's-complement pattern, and reading one as `uint64` and casting
gives 1.8e19, which then sizes a read or an allocation. `int32()` and `int64()`
do the narrowing once, with a range check. `unzigzag()` is written out because
the `n / 2 * sign` a reader might expect is wrong for negatives and produces
geometry that is mirrored rather than rejected.

Unknown fields are skipped by wire type, which is the forward-compatibility
rule of the spec. Group wire types are named so they can be recognised, but
`skip()` refuses them rather than guessing at their length. Every error carries
the byte offset at which it happened; for a tile in a viewport or a block in a
continental file that offset is the only way to find the failing message again.

{: .note }
`Error::Kind::Decompress` is not a protobuf concern. It lives here because both
formats this serves inflate before parsing, and splitting it out during the move
out of `libs/mvt` would have weakened the "this changed nothing" argument.

## Tests

| Target | Labels | Proves |
|---|---|---|
| `protowire_test_reader` | `protowire unit` | Varints, zigzag, tags, fixed-width fields and skipping decode from hand-written bytes; an overlong or truncated varint, a field number of zero, an invalid wire type, a group, and a length that overruns the buffer are refused; signed `int32`/`int64` survive their ten-byte encoding; an unaligned buffer reads correctly. |

This is the same file that used to be `mvt_test_reader`, required to pass
unchanged after the move.
