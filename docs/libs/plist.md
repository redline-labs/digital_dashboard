---
title: plist
parent: Libraries
---

# plist

## Overview

Apple property lists, in both wire formats this project meets: binary
(`bplist00`) on CarPlay's RTSP-style control channel, which is where this code
started out (it lived in `libs/airplay`), and XML on the usbmux socket and the
lockdown handshake. One `Value` model serves both, which is the point of having
it here rather than inside either consumer. It models the subset the stack
needs: dicts, arrays, ASCII and UTF-16 strings, raw data, integers, reals,
booleans and dates. Binary sets decode as arrays; UIDs and anything else
outside that subset are a decode failure, not a silent drop.

It replaced libplist, which came in with libimobiledevice when the CarPlay
stack was ported from LIVI. Both consumers, [apple_usb](apple_usb.html) and
[airplay](airplay.html), link it publicly; the node that uses all of them is
[carplay](../nodes/carplay.html), and the port's design notes are in
[carplay-port](../design/carplay-port.html). No sockets, no threads; the
only dependency is spdlog for decode diagnostics.

## Public headers

| Header | |
| --- | --- |
| `plist/value.h` | `plist::Value`, a tagged union with factory functions, fallback-returning accessors, and insertion-ordered dict access. `plist::Bytes` is `std::vector<uint8_t>`. |
| `plist/binary.h` | `encodeBinary`, `decodeBinary`, and `looksBinary`, which sniffs the `bplist00` magic. |
| `plist/xml.h` | `encodeXml` and `decodeXml`, in libplist's tab-indented layout. |

## Using it

Link the CMake target `plist`. Build a `Value` with the static factories,
encode with whichever codec the peer expects, and sniff before decoding
anything a peer sent, because lockdown and usbmux peers may answer in either
format. This is the shape of `LockdownClient::receivePlist` in
`libs/apple_usb/lockdown_client.cpp`:

```cpp
plist::Value request = plist::Value::dict();
request.set("Request", plist::Value::string("QueryType"));
request.set("Label", plist::Value::string(label));
const std::string xml = plist::encodeXml(request);

std::optional<plist::Value> reply = plist::looksBinary(body)
    ? plist::decodeBinary(body)
    : plist::decodeXml(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()));
if (const auto* error = reply ? reply->find("Error") : nullptr) {
    // error->asString()
}
```

Accessors take a fallback rather than throwing, so code parsing a phone's
payload never has to type-check first: `find` returns null for a missing key,
`at` returns a null `Value` for an out-of-range index, and `asInteger` on a
string returns the fallback.

## Behaviour worth knowing

`Value` is a hand-rolled tagged union rather than a `std::variant`, so that the
recursive container members only ever need `std::vector` of an incomplete
type, which is the one form the standard blesses.

Dicts are insertion ordered, and `set` on an existing key replaces it in
place. That is what makes encoding deterministic, and it is why
`test_binary` checks that key order survives a round trip.

`asReal` accepts an integer as well, because bplist writers freely mix the
two. `asDate` is seconds since the Apple epoch, 2001-01-01T00:00:00Z, not the
Unix one.

The XML encoder matches libplist byte for byte for scalars and containers,
which the peers reading it were written against. `<data>` is the one
exception: libplist wraps base64 at 68 columns and this wraps at 60. Both
parse either way, and the test suite asserts the match for everything else.

{: .note }
The XML decoder is deliberately tolerant of what real writers vary on (the
declaration, the DOCTYPE with or without an internal subset, comments,
whitespace, `<data>` split across lines) and deliberately intolerant of
anything else. An element outside the plist vocabulary is a protocol mismatch
worth failing on, so `<banana>` returns nullopt rather than being skipped.

`encodeBinary` returns an empty vector only for pathologically large inputs.
`decodeBinary` returns nullopt on a malformed or truncated buffer, and on any
object type outside the supported subset.

## Tests

```bash
ctest --test-dir build -L plist    # plist_test_binary, plist_test_xml, plist_test_libplist_vectors
```

All three are labelled `plist` and `unit`, and all three run on macOS: they
are round-trip and known-answer tests with no hardware behind them.

`plist_test_binary` round-trips a `GET /info` shaped payload and a
400-element array (which forces 2-byte object references), and decodes a
reference document produced by Apple's `plutil(1)`, so the binary format is
pinned to the real thing rather than to our own reading of it.

`plist_test_xml` weights known answers over round trips, because "we can read
what we wrote" proves nothing about a libplist peer. Its literal documents
were produced by libplist and by `plutil(1)`, and it covers the tolerance and
intolerance rules above.

`plist_test_libplist_vectors` carries documents captured from libplist 2.6.0
while it was still a vendored dependency. The differential test that generated
them went away with the dependency; this keeps what it proved. The direction
that matters is decode. Regenerating them needs libplist, which is gone, so
treat them as fixtures: a change to one wants justifying against a real
device.
