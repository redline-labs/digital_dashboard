// SPDX-License-Identifier: GPL-3.0-or-later
//
// A minimal I2C master abstraction with two backends:
//
//   * Linux  -- the kernel's /dev/i2c-N interface. On a bench setup the adapter
//               is the MCP2221A, bound by the in-kernel hid_mcp2221 driver; on a
//               deployed board it is the SoC's own controller. Same code either
//               way, which is the point.
//   * macOS  -- the MCP2221A driven directly over USB HID (hidapi), because
//               there is no kernel driver for it there.
//
// Backends are selected at compile time; see CMakeLists.txt.
#ifndef I2C_BUS_I2C_BUS_H_
#define I2C_BUS_I2C_BUS_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace i2c
{

// ---------------------------------------------------------------------------
// Every call is a single, complete I2C transaction terminated by a STOP.
//
// This matters for the MFi coprocessor: a register read is a write of the
// register address, a STOP, and then a *separate* read transaction. A combined
// write/read with a repeated START fails outright on this part -- verified on
// hardware, where `i2ctransfer w1@0x11 0x00 r1` errors while the same exchange
// split into two transactions works.
// ---------------------------------------------------------------------------
class Bus
{
  public:
    virtual ~Bus() = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool is_open() const = 0;

    // One write transaction. An empty payload is an address-only probe.
    virtual bool write(uint8_t address, const std::vector<uint8_t>& data) = 0;

    // One read transaction. Returns fewer bytes than requested on failure.
    virtual std::vector<uint8_t> read(uint8_t address, size_t length) = 0;

    // True when a device acknowledges its address. Note that some devices --
    // the MFi coprocessor among them -- ignore the first transaction after
    // idling, so a single negative probe does not prove absence.
    virtual bool probe(uint8_t address) = 0;

    // Addresses that acknowledged, scanned over the 7-bit range.
    virtual std::vector<uint8_t> scan();

    // Human-readable description of the underlying adapter, for logs.
    virtual std::string description() const = 0;
};

// Creates the platform's default backend. `hint` optionally selects a specific
// adapter: a device path such as "/dev/i2c-1" for the Linux backend, ignored by
// the hidapi backend. Empty means auto-detect.
std::unique_ptr<Bus> makeBus(const std::string& hint = {});

}  // namespace i2c

#endif  // I2C_BUS_I2C_BUS_H_
