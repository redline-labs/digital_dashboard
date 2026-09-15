---
title: i2c_bus
parent: Libraries
---

# i2c_bus

## Overview

A minimal I2C master abstraction with two backends, chosen at build time by
the host because that is the only thing it can follow. On Linux it is the
kernel's `/dev/i2c-N` interface: on a bench the adapter is an MCP2221A bound
by the in-kernel `hid_mcp2221` driver, on a deployed board it is the SoC's own
controller, and the same code serves both. Everywhere else it is the MCP2221A
driven directly over USB HID through [mcp2221a](mcp2221a.html), because macOS
ships no kernel driver for the chip. There is nothing to configure.

Every call is a single, complete transaction terminated by a STOP; there is
no combined write-then-read with a repeated START. The caller is
`libs/apple_mfi_ic`, the MFi authentication coprocessor.

## Public headers

| Header | |
| --- | --- |
| `i2c_bus/i2c_bus.h` | `i2c::Bus` (`open`, `close`, `write`, `read`, `probe`, `scan`, `description`) and `i2c::makeBus(hint)`. |

## Using it

Link the CMake target `i2c_bus`. The MFi driver asks the factory for the
platform's bus, opens it, and does a register read as two transactions:

```cpp
std::unique_ptr<i2c::Bus> bus = i2c::makeBus(hint);   // "" auto-detects
if (!bus || !bus->open()) { /* no adapter */ }

if (bus->write(kAddress, { reg })) {                 // write, STOP
    auto data = bus->read(kAddress, length);         // separate read, STOP
    if (data.size() < length) { /* the device did not answer */ }
}
```

`hint` is a device path such as `/dev/i2c-1` for the Linux backend and is
ignored by the HID backend. The MFi driver also honours `REDLINE_MFI_I2C_DEV`
when no hint is given.

## Behaviour worth knowing

The STOP between the write and the read is not a simplification. On the MFi
coprocessor a combined write/read with a repeated START fails outright,
verified on hardware: `i2ctransfer w1@0x11 0x00 r1` errors while the same
exchange split into two transactions works.

A single negative `probe()` does not prove absence. Some devices, the MFi
coprocessor among them, ignore the first transaction after idling. `scan()`
walks `0x03` to `0x77`, skipping the reserved ranges as `i2cdetect` does.

The Linux backend's auto-detect prefers the adapter whose sysfs name contains
`MCP2221` and falls back to the first bus it finds, because a deployed board
has unrelated buses (HDMI DDC, the PMIC, sensors) that must not be poked. The
HID backend fixes the bus speed at 100 kHz. A build with neither backend
compiled in gets `nullptr` from `makeBus` and an error in the log.

## Tests

None of its own; `libs/i2c_bus` has no test directory. The Linux backend is
exercised by whatever drives the MFi coprocessor on a board, and the HID
backend by the same code on a Mac.
