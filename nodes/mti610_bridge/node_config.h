// SPDX-License-Identifier: GPL-3.0-or-later
//
// The mti610 node's YAML configuration.
//
// Same shape as nodes/bd992_bridge and nodes/can_bridge: plain structs with
// in-class defaults, a parse that accumulates every error rather than stopping
// at the first, and a string-taking overload so the parser is testable without
// a file.

#ifndef MTI610_NODE_CONFIG_H
#define MTI610_NODE_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

#include "mti610/output_config.h"
#include "xbus/data_id.h"

namespace mti610_node
{

// One output the device should emit, and how often.
struct OutputEntry
{
    // The base identifier, format nibble cleared. The precision below is what
    // fills it in -- keeping them apart means the YAML says
    // `{ data: acceleration, precision: fp1632 }` rather than `0x4022`.
    xbus::DataId data { xbus::DataId::Acceleration };

    xbus::Precision precision { xbus::Precision::Fp1632 };

    // Hertz. Zero and 65535 both mean "as fast as the device can", and the
    // device forces the latter on anything that accompanies every packet.
    std::uint16_t frequencyHz { 0 };

    // The wire identifier this row asks for.
    std::uint16_t rawDataId() const;
};

struct DeviceConfig
{
    // /dev/ttyUSB0 on Linux, /dev/cu.usbserial-XXXX on macOS.
    std::string port;

    // 115200 is the factory default in serial mode.
    //
    // THIS NODE NEVER CHANGES THE DEVICE'S BAUD RATE -- it only opens the port
    // at the rate the device is already using. Changing it needs SetPortConfig,
    // whose word layout the LLCP documents only as an image. See
    // docs/nodes/mti610_bridge.md.
    std::uint32_t baud { 115200 };

    std::uint32_t openTimeoutMs { 2000 };

    // Tried in order, then the last repeats. Capped rather than doubling
    // forever so an adapter replugged after an hour is picked up in seconds.
    std::vector<std::uint32_t> reopenBackoffMs { 250, 500, 1000, 2000, 5000 };
};

struct ConfigurationConfig
{
    // enforce      read the device's output configuration, then write only
    //              what differs.
    // report_only  read and report the difference, change nothing.
    mti610::ConfigMode mode { mti610::ConfigMode::Enforce };

    // additive   leave outputs that are not listed alone, and report them.
    //            Since SetOutputConfiguration replaces the whole list, this
    //            means the node RE-SENDS them -- see
    //            libs/mti610/include/mti610/output_config.h.
    // exclusive  turn off anything not listed. Only for a device this node
    //            owns outright.
    mti610::PortPolicy policy { mti610::PortPolicy::Additive };

    std::uint32_t replyTimeoutMs { 1000 };
    std::uint32_t retries { 2 };

    // How often to re-read and re-compare. Zero checks once, at startup.
    //
    // A RE-CHECK STOPS THE DATA for as long as it takes, because an MTi
    // answers configuration messages only in Config state and emits MTData2
    // only in Measurement state. That is a property of the device, not of this
    // node, and it is why the default is a minute rather than a second.
    std::uint32_t recheckIntervalS { 60 };

    std::vector<OutputEntry> outputs;
};

struct PublishConfig
{
    std::string topicPrefix { "nodes/mti610" };
    std::string statusKey { "nodes/mti610/status" };
    std::uint32_t statusIntervalMs { 1000 };

    // Publish items this build does not model on <prefix>/mtdata2/raw. Worth
    // leaving on: a device emitting something unrecognised is otherwise
    // indistinguishable from one that is silent, and on an MTi-610 a busy raw
    // topic means the device is not a 610.
    bool publishUnknownItems { true };
};

struct NodeConfig
{
    DeviceConfig device;
    ConfigurationConfig configuration;
    PublishConfig publish;
};

// Both report every problem they find before returning false, so a config with
// three mistakes takes one run to fix rather than three.
bool parse_node_config(const std::string& yaml, NodeConfig& out);
bool load_node_config(const std::string& path, NodeConfig& out);

// The desired list, in the form libs/mti610 compares against.
std::vector<mti610::OutputEntry> to_output_entries(const std::vector<OutputEntry>& outputs);

// Every value the `data:` key accepts, for an error message that tells the
// reader what to write instead of what they wrote.
std::string known_output_names();
std::string known_precision_names();

} // namespace mti610_node

#endif // MTI610_NODE_CONFIG_H
