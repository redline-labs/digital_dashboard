---
title: dbc_parser
parent: Libraries
---

# dbc_parser

## Overview

DBC files parsed into an AST with line-and-column diagnostics, and typed C++
decoders and encoders generated from that AST at build time. Three targets:
`dbc_parser` (alias `dbc::parser`) is the lexer, parser and AST; `dbc_codegen`
(alias `dbc::codegen`) turns a parsed database into source text as a pure
function; `dbc_code_gen` is the command-line tool on top, and the
`generate_dbc_code()` CMake function runs it. There is no logging dependency
on purpose: the parser returns its diagnostics rather than printing them, so a
caller decides what to do about a bad file.

It knows nothing about CAN hardware. The generated code takes an identifier and
a span of bytes and hands back a struct; feeding it frames is the caller's job,
and the callers in the tree are [msel](msel.html) and the nodes under
`nodes/`. A DBC the parser only half understands is rejected outright, because
a half-understood DBC produces a decoder that is confidently wrong.

## Public headers

| Header | |
| --- | --- |
| `dbc_parser/ast.h` | `Database`, `Message`, `Signal`, `ValueMapping`, `SignalValueType`: what a file says, with the extended-id flag already stripped. |
| `dbc_parser/dbc_parser.h` | `Parser`: `parse()` returns `optional<Database>`, `diagnostics()` says why not; `isUsableIdentifier`. |
| `dbc_parser/dbc_lexer.h` | `Lexer`, `Token`, `TokenKind`; positions recorded at the character itself. |
| `dbc_parser/diagnostic.h` | `Diagnostic`, `Severity` and the `Diagnostics` collector, so one run reports everything wrong with a file. |
| `dbc_parser/generate_h.h` | `generate_sources` (files in memory, no filesystem), `write_sources`, and the individual emitters. |

## Using it

Generate a library from a DBC in CMake and link it; nothing links `dbc_parser`
directly except the tools and tests:

```cmake
generate_dbc_code(dbc_msel_master_relay ${CMAKE_CURRENT_SOURCE_DIR}/msel_master_relay.dbc)
```

That produces `<name>.h`, `<name>_common.h`, `<name>_parser.h`,
`<name>_parser.cpp` and one header per message, and a static library of the
same name. A message struct decodes itself, and the parser class dispatches by
identifier to accumulated handlers:

```cpp
dbc_msel_master_relay::Master_Relay_Status_t msg;
if (!msg.decode(data)) { /* too short: nothing was touched */ }

dbc_msel_master_relay_parser parser;
parser.on_Master_Relay_Status([&](const auto& m) { /* ... */ });
parser.handle_can_frame(frame.id, frame.data);
```

Cross builds run the generator through `cmake/NativeCodegen.cmake`, since the
copy built for the target cannot run on the host.

## Behaviour worth knowing

`Message::id` has the DBC extended-frame flag stripped and `isExtended` set
beside it. Comparing a raw DBC id against an id from a CAN driver matches
nothing, because only the file sets bit 31.

Anything that would change what the generated code decodes is an error, not a
warning. The failure this replaced was a build that silently dropped a
message and exited 0: one typo'd `SG_` line deleted a whole `BO_` and
everything under it, and the only clue was a node later failing to compile
against the vanished symbol. `CM_`, `VAL_`, `SIG_VALTYPE_` and `BA_` refer back
to a `BO_` by id, so they are collected while scanning and applied once the
whole file is read; section order does not matter and a dangling reference
says "no such signal" instead of vanishing. Names that are C++ keywords or
contain punctuation are rejected here, not inside a generated header.

`SIG_VALTYPE_` matters and is silent when wrong: an IEEE float read as an
integer is a plausible-looking number, not an error. A signal can be both
multiplexed and a multiplexor (`m3M`); nested multiplexing is reported by
name, and extended multiplexing is refused rather than mis-decoded.

In the generated code a short frame is rejected and leaves already decoded
values alone; a caller that zero-padded used to decode the padding as
readings. Handlers accumulate, so an aggregator cannot silently discard a
directly registered handler for the same message. A multiplexed message has
two handler kinds: `on_X` fires once per complete batch and
`on_X_each_frame` once per group, for a device that sends some groups
conditionally. `decode()` and `encode()` are usable in constant expressions,
and an unsigned field with a negative offset gets a signed type.

{: .note }
The vendor DBCs under `dbcs/` are parsed every build, and `dbc_code_gen`
rejects out-of-frame signals, duplicate ids, missing multiplexors and unusable
names, so a bad file there fails the build. Deleting a generated per-message
header by hand also fails loudly, because the main header includes it.

## Tests

```bash
ctest --test-dir build -L dbc_parser
```

Four targets, all labelled `dbc_parser` and `unit`, gated behind
`BUILD_TESTING` because a consumer that wants only a DBC parser should not
have to stub `add_project_test`.

`dbc_parser_test_golden` is a differential test against cantools. The inputs
under `libs/dbc_parser/tests/dbcs/` are synthetic, generated by
`tests/gen_golden.py` as a systematic sweep over byte order, signedness,
length, bit alignment and scaling, and the expected values are cantools',
embedded in `tests/golden_data.h`: 2640 decode cases and 2640 encode cases.
cantools is not a build dependency, and everything the script writes is
checked in; regenerate only when changing the sweep. The tests deliberately do
not read the product DBCs under `dbcs/`, so editing a vendor file cannot break
a parser test. That trade is only sound while the sweep stays generated
rather than hand-picked: a round-trip bug in a real DBC has to be impossible
unless the corpus would also have caught it. A few cases are pinned at compile
time with `static_assert`, so a regression in the bit walk is a build failure.

`dbc_parser_test_generated_api` covers what a differential test cannot see,
since it only compares numbers: frame-length checks, handler registration,
multiplex gating and the aggregator. `dbc_parser_test_malformed` feeds
embedded bad files to the parser alone and checks each is rejected with the
expected fragment; every case is a failure mode the old parser had, including
the missing multiplexor that took the generator down with a segfault.
`dbc_parser_test_codegen` drives the generator in process from embedded DBC
strings and checks the shape of the text: string and unit escaping,
enumerator naming, which C++ type a signal gets, extended identifiers, and
that dispatch is a switch. `dbc_parser_parse` is a parse-and-print executable,
not a registered test.
