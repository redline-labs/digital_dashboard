---
title: Nodes
nav_order: 3
---

# Nodes

A node is a single-purpose program that puts one piece of hardware (or one
file) on the bus, plus the two tools that look at the bus. Every one under
`nodes/` is listed here, whether or not it has a page yet. Each node's
configuration lives under `configs/<node>/` where it has one.

Nodes that decode CAN frames do not open the adapter themselves: `can_bridge`
owns the hardware and publishes raw frames, and the decoders subscribe to those.

| Node | What it does | Needs | Page |
|---|---|---|---|
| `backlight` | A display module's backlight and ambient light and temperature sensors on the bus; brightness as a service. | the rootfs display record and its sysfs devices | [backlight](backlight.html) |
| `bag` | Records the bus to an MCAP file and replays it with the original timing. | nothing | [bag](bag.html) |
| `bd992_bridge` | Trimble BD992 GNSS: GSOF records onto topics, the receiver's configuration as services. | a receiver on the network | [bd992_bridge](bd992_bridge.html) |
| `bd992_mock` | Drives a route or a race track and publishes GNSS the way a receiver would. | map data | [bd992_bridge](bd992_bridge.html#without-a-receiver) |
| `can_bridge` | CAN hardware onto zenoh topics and back: PCAN, SocketCAN, the MoTeC UTC, and `.trc` traces; records taps as traces. | a CAN adapter, or a trace to replay | [can_bridge](can_bridge.html) |
| `carplay` | Wired CarPlay: owns the USB, iAP2 and AirPlay session with the phone and publishes video, audio, input and metadata. | an iPhone and an MFi coprocessor on Linux, or `--simulate` anywhere | [carplay](carplay.html) |
| `grayhill_keypad` | A Grayhill 3K CANopen keypad: keys in, LEDs out; a separate one-shot tool reconfigures the keypad. | the keypad on CAN | [grayhill_keypad](grayhill_keypad.html) |
| `inspect` | Look at the bus: list topics, read a stream, describe a schema, call a service. | nothing | [inspect](inspect.html) |
| `map_match` | Which road the vehicle is on, and what is ahead of it, from GNSS and the road graph. | a road graph file | [map_match](map_match.html) |
| `map_server` | Serves `.mbtiles` archives over zenoh: tiles, catalogs and style assets. | the archives | [map_server](map_server.html) |
| `megasquirt` | Megasquirt dash CAN frames into typed telemetry. | `can_bridge` | [megasquirt](megasquirt.html) |
| `motec_ltc` | MoTeC LTC lambda modules over CAN. | `can_bridge` | [motec_ltc](motec_ltc.html) |
| `motec_m1` | MoTeC M1 ECU CAN telemetry. | `can_bridge` | [motec_m1](motec_m1.html) |
| `motec_pdm` | MoTeC PDM generic output frames into typed telemetry. | `can_bridge` | [motec_pdm](motec_pdm.html) |
| `msel_master_relay` | The MSEL solid state battery isolator: telemetry out, its settings as services. | `can_bridge` | [msel_master_relay](msel_master_relay.html) |
| `mti610_bridge` | Xsens MTi-610 IMU on a serial port: MTData2 items onto topics, output configuration as services. | the device, or a capture to replay | [mti610_bridge](mti610_bridge.html) |
| `racegrade_tc8` | RaceGrade TC8 thermocouple amplifier over CAN. | `can_bridge` | [racegrade_tc8](racegrade_tc8.html) |
| `state_estimator` | Fuses the MTi-610's increments and the BD992's dual-antenna fixes into position, attitude, velocity, acceleration and sideslip at 100 Hz; a fixed-lag smoother that estimates the lever arm and boresight as it drives. | `mti610_bridge` and `bd992_bridge` (GSOF 1, 2, 8, 12, 27, 38) | [state_estimator](state_estimator.html) |
| `web_console` | The board's web UI: reflash, system info, node health and service calls, from a browser. | nothing (RAUC for reflashing) | [web_console](web_console.html) |
| `xpr_bridge` | A Motorola MOTOTRBO radio: what it says about itself onto topics, the channel as a service. | the radio on the network | [xpr_bridge](xpr_bridge.html) |
