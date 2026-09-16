---
title: pub_sub
parent: Libraries
---

# pub_sub

## Overview

The bus library: zenoh in peer mode carrying Cap'n Proto messages. It owns the
one session per process, the typed publisher and subscribers, services and
their clients, the expression evaluator that turns a message into a number,
discovery through liveliness, the topic key rules, the sample timestamp
conversion, and generic capnp-to-JSON over the dynamic API. Every rule in
[Bus conventions](../reference/bus-conventions.html) is implemented here and
nowhere else.

It has no Qt in it; zenoh callbacks arrive on zenoh threads and hopping to the
GUI is the caller's job, with `dashboard/expression_subscription.h` as the
established shape. The schema registry, `pub_sub/schema_registry.h`, is
generated in `schemas/` by `tools/schema_registry_plugin.cpp` and reaches this
library through the `schemas` target; nothing about the schema list is known
here, so the two halves cannot disagree as they once did.

## Public headers

| Header | |
| --- | --- |
| `pub_sub/zenoh_publisher.h` | `ZenohPublisher<SchemaT>`: owns a builder, `fields()`, `put()`, subscriber presence. |
| `pub_sub/zenoh_subscriber.h` | `ZenohTypedSubscriber<SchemaT>`: each sample as a typed `Reader`. Also pulls in the expression subscriber. |
| `pub_sub/expression_subscriber.h` | `ZenohExpressionSubscriber`: subscribe, decode, evaluate an expression. The lean header widgets use. |
| `pub_sub/expression_evaluator.h` | `ExpressionEvaluator`: decode and evaluate with no zenoh in it. |
| `pub_sub/raw_subscriber.h` | `RawSubscriber`: payload bytes plus schema name, or `SampleInfo` with key, publish time and origin. |
| `pub_sub/zenoh_service.h` | `ZenohService<Req, Resp>`: a queryable with a handler, advertised on liveliness. |
| `pub_sub/zenoh_client.h` | `ZenohClient<Req, Resp>`: a blocking GET, first reply wins. For tools. |
| `pub_sub/zenoh_async_client.h` | `ZenohAsyncClient<Req, Resp>`: the same without blocking; callback exactly once. |
| `pub_sub/dynamic_service_call.h` | `callService()` / `callServiceBlocking()`: request from JSON, schema known by name, every reply collected. |
| `pub_sub/session_manager.h` | `SessionManager`: `getOrCreate()`, `insertConfig()`, `zid()`, `isOpen()`, `shutdown()`. |
| `pub_sub/node_identity.h` | `NodeIdentity`: one liveliness token naming this process. Declare one in every `main()`. |
| `pub_sub/topic_key.h` | The key charset, `isValidTopicKey`, `topicKeyProblem`, mangling, and the `@redline/...` key spaces. |
| `pub_sub/topic_directory.h` | `TopicDirectory`, `NodeDirectory`, `ServiceDirectory`: what is advertised, kept current. |
| `pub_sub/topic_discovery.h` | `observeTopics()` and `readOneSample()`: what is flowing, over a window. |
| `pub_sub/timestamp.h` | `ntp64ToUnixNanos()` and its inverse, with what the clock does and does not mean. |
| `pub_sub/capnp_encoding.h` | `kCapnpEncodingMime` and `schemaNameFromEncoding()`. |
| `pub_sub/schema_layout.h` | `layoutHash()`, `layoutHashFor()`, `layoutHashOfDescriptor()`: which revision of a schema some bytes were written against. Registered schemas are fingerprinted by the registry generator at build time; `layoutHashFor()` is a table lookup. |
| `pub_sub/capnp_payload.h` | `WordAlignedPayload`: word-aligned capnp access to any byte buffer. |
| `pub_sub/zenoh_payload.h` | `ZenohPayload`: the same over a live `zenoh::Bytes`, borrowing rather than copying. |
| `pub_sub/can_frame.h` | `fromCapnp()` and `toCapnp()` between the `CanFrame` schema and `helpers::CanFrame`. The length copied is what was both declared and supplied, capped at 64. |
| `pub_sub/capnp_json.h` | `capnpToJson`, `jsonToCapnp`, `describeSchema`, `fixedListLength`. Unknown field names are errors. |
| `pub_sub/detail/` | `BytePublisher` and `ByteSubscriber`: the zenoh boundary. The seam, not the API. |

## Using it

Link the CMake target `zenoh_pub_sub`. A node declares its identity first,
then publishers and subscribers on concrete keys:

```cpp
#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"

int main(int argc, char** argv)
{
    pub_sub::NodeIdentity node("backlight");

    pub_sub::ZenohPublisher<EngineRpm> pub("vehicle/engine/rpm");
    pub.fields().setRpm(1234);
    pub.put();

    pub_sub::ZenohTypedSubscriber<EngineRpm> sub("vehicle/engine/rpm",
        [](EngineRpm::Reader rpm) { /* zenoh thread; the Reader dies with the callback */ });
}
```

Every constructor opens the shared session on first use. Check `isValid()`:
an object that failed to declare delivers nothing and never says why.

## Behaviour worth knowing

**Threads.** Every callback runs on a zenoh RX thread. It must not block and
must not throw: the frame above it is Rust, and an exception crossing it
aborts the process. The library catches whatever escapes anyway. Qt owns
exactly one thread; hop with
`QMetaObject::invokeMethod(obj, lambda, Qt::QueuedConnection)`.

`ZenohPublisher` is not thread-safe, deliberately: there is one builder inside.
Give each thread its own or hold a lock across `fields()` and `put()`. `put()`
re-roots the builder, so a reference from `fields()` held across it refers to
the next message. `hasSubscribers()` is a boolean, so the presence handler
fires on the first subscriber arriving and the last leaving, not in between.

{: .warning }
Decoding against the wrong schema is silent: field offsets land on different
bytes and produce a plausible number, not an exception. Publishers stamp
`application/capnp;<Schema>` on every sample and subscribers check it.
`ExpressionEvaluator::checkPublishedSchema()` takes the whole encoding string,
not the schema half, and complains once.

A payload that is not a whole number of 8-byte words is refused, because capnp
reads a short buffer as a message whose fields are all default. `evaluate<T>()`
returns `std::nullopt` for an unusable sample rather than 0.0, which used to
drive an oil-pressure gauge to zero on a corrupt packet. `WordAlignedPayload`
and `ZenohPayload` make word alignment true by construction instead of by the
accident that `operator new` aligns.

**Keys** are `[A-Za-z0-9_-/]`, checked with `topicKeyProblem()` in the editor,
at config load and in the publisher. `%` is the mangling separator, `@` makes a
segment invisible to every wildcard, and `* $ ? #` fail to construct.
`NodeIdentity` refuses a name that is not a usable segment.

**Liveliness.** Three key spaces, `@redline/adv`, `@redline/node` and
`@redline/svc`, join on the zid, and every parser accepts extra trailing
segments so an older build does not go dark when a field is added.
`TopicDirectory` never removes an entry, only marks it unreachable, and an
empty `owner_zid` means unknown, not unowned. A token says a process is up,
not that data flows; `observeTopics()` answers that, and its empty result
means "nothing published during the window", since zenoh retains nothing.

**Timestamps.** `SessionManager::buildConfig()` enables timestamping, which
zenoh only does in router mode by default. The stamp is an NTP64,
`seconds << 32 | fraction`; a naive read is off by 2^32 and looks like a
timestamp until plotted. It is the publisher's wall clock, re-stamped rather
than rejected when too far ahead, and it runs out in 2036.

**The session.** One per process. `insertConfig()` affects the next session
opened, which is why [cli](cli.html) applies `--connect` and `--mode` before
any verb runs. The manager stores settings rather than a `zenoh::Config`,
because `Session::open()` moves out of its config and touching it again
aborted the process. `isOpen()` reports without opening.
`PUB_SUB_NO_DISCOVERY=1` keeps a session off the machine's bus; every `net`
test sets it.

**Services.** A handler that throws answers that caller with an error reply
instead of taking the node down. `ZenohAsyncClient` fires its callback exactly
once, first reply wins, and cannot tell an unserved key from a timeout because
the wire cannot. `callService()`, shared by `inspect call` and switchboard,
collects every reply so a key served by two nodes is visible.

**Header cost.** `<zenoh.hxx>` is about 89,000 preprocessed lines and stays
out of the publisher and subscriber headers behind `detail::BytePublisher` and
`ByteSubscriber`; `expression_evaluator.h` hides exprtk the same way. Widgets
include `expression_subscriber.h`, the lean header; `zenoh_subscriber.h` adds
capnp, and the service and client headers include zenoh itself.

## Tests

Every `net` test opens a real session and skips itself, exit 0 with a warning,
on a host where none can be opened.

| Target | Labels | Proves |
| --- | --- | --- |
| `pub_sub_test_capnp_encoding` | `pub_sub unit` | The publish-side encoding string matches the subscribe-side registry lookup, through the real encoder. |
| `pub_sub_test_capnp_json` | `pub_sub unit` | JSON to capnp and back over a fixture with every field shape, parsed at test time so it never appears in a picker. |
| `pub_sub_test_can_frame` | `pub_sub unit` | The `CanFrame` conversion: a declared length and a supplied payload that disagree come out as the bytes that exist. |
| `pub_sub_test_timestamp` | `pub_sub unit` | The NTP64 conversion, where every wrong answer is a plausible number. |
| `pub_sub_test_schema_layout` | `pub_sub unit` | The schema fingerprint: every generated constant matches the schema this build links, distinct for every schema in the registry, and the same computed from a stored descriptor as from the compiled schema. |
| `pub_sub_test_topic_key` | `pub_sub unit` | Key validation and mangling, round-tripped over every key in `configs/`. |
| `pub_sub_test_expression_evaluator` | `pub_sub unit` | Decode and evaluate against `ExpressionEvaluator` directly, with no session. |
| `pub_sub_test_sample_metadata` | `pub_sub net` | Zenoh stamps our samples at all, which only a real bus can say. |
| `pub_sub_test_session_manager` | `pub_sub net` | Releasing one session and opening another. |
| `pub_sub_test_expression_eval` | `pub_sub net` | The same decode path through a live subscriber; kept unchanged as evidence the evaluator split changed nothing. |
| `pub_sub_test_publisher_reuse` | `pub_sub net` | Consecutive messages either side of the scratch buffer growing arrive intact. |
| `pub_sub_test_topic_directory` | `pub_sub net` | A topic appears from the publisher alone, with no sample published. |
| `pub_sub_test_async_client` | `pub_sub net` | The callback fires exactly once in every outcome, unserved key and timeout included. |
| `pub_sub_test_dynamic_service_call` | `pub_sub net` | Every reply collected, error replies surfaced, a bad request refused before sending, a throwing handler answering with an error. |
