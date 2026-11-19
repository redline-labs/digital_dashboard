// The M1 hardware description: what a board's I/O is, where it is on the connectors, which driver device and channel
// serve it, and the hardware functions the firmware calls to use it.
//
// The family-wide part (io features with their methods, properties and functions; io types; device types; memory
// sections) is shared by every M1 ECU; the board part (connectors, the I/O, the devices' channel maps, the task events)
// is one ECU's. The tables (m150.cpp) are the source of truth and are maintained by hand: they were first derived from
// the M1 system 01.04.00 hardware description, whose file is not in the tree. Nothing here is executed: the board
// (board/) builds its stubs from it and the agent methods describe the hardware with it.
//
// A function's `index` is the low byte of the hardware-function command word the firmware passes (byte 1 is 'A', the
// argument types are in bits 16 and up), so a call can be named from its command alone. Most system-library ("usr")
// functions carry no index (kNoIndex); a few (the test functions, the peak-charge helpers) are numbered all the same.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace sim::m150::topology {

inline constexpr int kNoIndex = -1;

enum class Feature : uint8_t { Din, Dout, Ain, Cfg, Core };

std::string_view feature_name(Feature feature);

// A value a method or feature defines: a function's argument or return, or a channel the firmware reads.
struct Property {
    std::string_view id;
    std::string_view data_type;  // "f32", "s32", "u32"
    std::string_view unit;       // "V", "Hz", "enum", ...
    std::string_view display;
    std::string_view constant;   // "const": a fixed value, empty otherwise
    std::string_view mask;       // the bit mask a masked property sits in, empty otherwise
};

// A named value of an enumerated property; `mask` names the property_mask group it belongs to (empty if none).
struct EnumValue {
    std::string_view property;
    std::string_view id;
    std::string_view display;
    int64_t value;
    std::string_view mask;
};

// A masked field of a property (`value` is the mask).
struct PropertyMask {
    std::string_view property;
    std::string_view id;
    std::string_view display;
    int64_t value;
};

// The allowed range or default of a property (as written in the description; numbers are left as text).
struct PropertyInfo {
    std::string_view property;
    std::string_view min;
    std::string_view max;
    std::string_view mode;
    std::string_view default_value;
    std::string_view enum_default;
};

struct Argument {
    std::string_view type;      // "value" (in), "out"
    std::string_view property;  // the method property it carries
};

struct Function {
    std::string_view suffix;  // the function's name after the method's ("periodic_cfg")
    std::string_view display;
    std::string_view use;    // "init", "default", "exit", or empty
    std::string_view order;  // "cfg", "resource", or empty
    std::string_view code;   // "dev" (a hardware function) or "usr" (the system library)
    int index;               // the hardware-function number, or kNoIndex
    std::span<const Argument> arguments;
    std::string_view return_type;      // "error", "handle", "property", or empty
    std::string_view return_property;  // for "property"
    std::span<const std::string_view> schedule_events;  // task events it may run at ("1MS", ...)
    std::span<const std::string_view> hooks;            // event hooks it runs from ("initcall_cfg", ...)
    std::span<const std::string_view> after;            // methods whose function of the same hook runs first
};

struct Method {
    Feature feature;
    std::string_view id;
    std::string_view display;
    int index;
    int resources;
    std::span<const Property> properties;
    std::span<const EnumValue> enums;
    std::span<const PropertyMask> masks;
    std::span<const PropertyInfo> info;
    std::span<const Function> functions;
    std::span<const std::string_view> requires_methods;
};

struct FeatureDef {
    Feature feature;
    std::string_view id;
    std::string_view display;
    int priority;
    std::span<const Property> properties;
    std::span<const Method> methods;
};

struct Characteristic {
    std::string_view id;
    std::string_view value;
};

struct CharacteristicDefn {
    std::string_view id;
    std::string_view data_type;
    std::string_view unit;
    std::string_view display;
};

struct Implements {
    Feature feature;
    std::span<const Feature> requires_features;
};

struct IoType {
    std::string_view id;
    std::string_view display;
    std::string_view adc;
    std::string_view out;
    std::span<const Implements> implements;
    std::span<const Characteristic> characteristics;
    std::span<const PropertyInfo> info;
};

struct DeviceType {
    std::string_view id;  // "eqadc", "ad7490", "emios", "tpu", "comms"
    std::string_view display;
    int channels;
    char magic;  // the driver device's byte in an I/O resource number ('a'..'e')
};

struct MemorySection {
    std::string_view id;  // ecu_cal, ecu_data, ...
    int index;            // the XCP address extension
};

// ---------------------------------------------------------------------------------------------- one ECU

struct Connector {
    std::string_view id;  // "A".."D"
    std::string_view display;
    std::string_view pcb;
    std::string_view device;
    std::string_view loom;
};

struct Pin {
    std::string_view connector;
    int position;
};

// An I/O made of others (a bridge's two halves, a knock input's two pins).
struct SubIo {
    Feature feature;
    int index;
    std::string_view display;
    std::string_view io;
};

struct Io {
    std::string_view id;
    std::string_view display;
    std::string_view group;  // the iogroup's id, empty if it has none
    std::string_view io_type;
    std::span<const Pin> pins;
    std::span<const SubIo> parts;
    std::span<const PropertyInfo> info;
};

struct MethodRef {
    Feature feature;
    std::string_view method;
};

// A channel of a driver device and what it serves: an I/O, or a whole I/O group (a group's shared configuration, as
// "INJ_PH_GLOBAL"; a bus, as "CAN1" with its _HI and _LO pins).
struct DeviceChannel {
    Feature feature;
    int index;
    std::string_view io;  // an Io's id or an iogroup's
};

// Channels of a device that offer the same methods.
struct DeviceGroup {
    std::span<const MethodRef> modes;
    std::span<const DeviceChannel> channels;
};

struct Device {
    std::string_view device_type;
    std::span<const DeviceGroup> groups;
};

struct Event {
    std::string_view id;  // "1MS".."1S"
    std::string_view display;
    double period_s;
    int priority;
    int index;  // the task trigger bit
};

struct MemorySize {
    std::string_view section;
    uint64_t bytes;
};

struct FpgaImage {
    std::string_view version;
    std::string_view type;
    std::string_view file;
};

struct Board {
    std::string_view id;  // "ecu120"
    std::string_view display;  // "M150"
    int board;                 // the hardware board number
    std::span<const Characteristic> characteristics;  // sys_clock, sys_tick_shift, timer_clock
    std::span<const EnumValue> enums;
    std::span<const PropertyInfo> info;
    std::span<const Connector> connectors;
    std::span<const Io> ios;
    std::span<const Device> devices;
    std::span<const Event> events;
    std::span<const std::string_view> event_hooks;
    std::span<const MemorySize> memory_sizes;
    std::span<const FpgaImage> fpga_images;
};

struct Topology {
    std::string_view version;  // the hardware description's major.minor.release
    std::span<const CharacteristicDefn> characteristic_defns;
    std::span<const IoType> io_types;
    std::span<const FeatureDef> features;
    std::span<const DeviceType> device_types;
    std::span<const MemorySection> memory_sections;
    Board board;

    // Lookups (linear: the tables are small and these are not on a hot path). Null when absent.
    const Io* io(std::string_view id) const;
    // True if `id` names an iogroup of the board (the group of some Io).
    bool has_group(std::string_view id) const;
    const IoType* io_type(std::string_view id) const;
    const DeviceType* device_type(std::string_view id) const;
    const Method* method(Feature feature, std::string_view id) const;
    // The hardware function with this number, and the method it belongs to.
    const Function* function(int index, const Method** method = nullptr) const;
    std::string_view characteristic(std::string_view id) const;
};

// The M150 (ecu120), from M1 system 01.04.00 (generated: board/topology/m150.cpp).
const Topology& m150();

}  // namespace sim::m150::topology
