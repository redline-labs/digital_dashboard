---
title: byte_stream
parent: Libraries
---

# byte_stream

## Overview

The byte stream the BD992, MTi and XPR transports are written against, the
TCP and replay streams under it, and the backoff their reconnect loops share.
Framers, parsers and command exchanges above it never see a socket or a tty,
which is what lets `--replay` feed a captured file through the same code the
live device uses and lets tests drive a scripted peer.

Each device library used to carry its own copy of all of this, so a fix to
connecting reached one of three. It holds no device code, so linking it
borrows an interface and nothing else. `apple_usb::ByteStream` is separate on
purpose: it is a pointer-and-length API the TLS stack is written against.

## Public headers

| Header | |
| --- | --- |
| `byte_stream/byte_stream.h` | `ByteStream`: `sendAll`, `recvSome`, `isOpen`, `close`. |
| `byte_stream/tcp_stream.h` | `TcpStream::connect(host, port, timeout)`: a client-only stream over TCP, `AF_UNSPEC`, every resolved address tried, a non-blocking connect with a timeout. |
| `byte_stream/replay_stream.h` | `ReplayStream`: a file of captured bytes handed out in small chunks, optionally looping. |
| `byte_stream/backoff.h` | `Backoff`: the wait before the next connection attempt. `sleepWhile()`: a wait that ends promptly on stop. |
| `byte_stream/error.h` | `byte_stream::Error`: `NotFound`, `ConnectFailed` or `Io`. Each device library maps it with `from_stream()`. |

## Behaviour worth knowing

`recvSome` returns `>0` for bytes, `0` for a timeout with nothing available
and `-1` for an error or a closed peer. The two must stay distinct: a caller
polling for data treats `0` as "not yet" and would spin forever on a dead link
if a closed peer also reported `0`.

`Backoff` steps along its schedule on every failure -- an attempt that did not
connect, or a connection that ended -- and stays on the last entry. Only a
connection that stayed up for `stableAfter` (ten seconds by default) starts it
over. Resetting on connect is what let a peer that accepts and closes at once
be reconnected about ten thousand times a second.

`ReplayStream`'s chunk size is small by default. Handing a framer the whole
file in one call tests a case that never happens on a socket.

## Tests

```bash
ctest --test-dir build -L byte_stream    # byte_stream_test_backoff, byte_stream_test_streams
```
