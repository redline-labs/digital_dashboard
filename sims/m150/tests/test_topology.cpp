// The M150's generated topology (board/topology/m150.cpp) is whole and consistent: the counts the hardware description
// has, and every name one table uses resolves in another -- an I/O's type and connector, a device channel's I/O, a
// device mode's method, a function argument's property, a part's I/O. Generated code that dropped or misspelt one would
// leave a stub with nothing behind it.
#include <set>
#include <string_view>

#include "m150/board/topology/topology.h"
#include "mctest/test.h"

using namespace sim::m150::topology;

namespace {

bool has_property(const Method& method, const FeatureDef& feature, std::string_view id) {
    for (const Property& p : method.properties) {
        if (p.id == id) {
            return true;
        }
    }
    for (const Property& p : feature.properties) {
        if (p.id == id) {
            return true;
        }
    }
    return false;
}

const FeatureDef* feature_def(const Topology& t, Feature f) {
    for (const FeatureDef& def : t.features) {
        if (def.feature == f) {
            return &def;
        }
    }
    return nullptr;
}

}  // namespace

TEST(topology_m150_board_identity_and_counts) {
    const Topology& t = m150();
    CHECK(t.board.id == "ecu120");
    CHECK(t.board.display == "M150");
    CHECK_EQ(t.board.board, 5);
    CHECK(t.characteristic("sys_clock") == "125000000");
    CHECK(t.characteristic("sys_tick_shift") == "8");
    CHECK(t.characteristic("nonexistent").empty());
    CHECK_EQ(t.board.connectors.size(), 4);
    CHECK_EQ(t.board.ios.size(), 287);
    CHECK_EQ(t.board.devices.size(), 5);
    CHECK_EQ(t.board.events.size(), 10);
    CHECK_EQ(t.board.event_hooks.size(), 4);
    CHECK_EQ(t.io_types.size(), 35);
    CHECK_EQ(t.features.size(), 5);
    CHECK_EQ(t.device_types.size(), 5);
    CHECK_EQ(t.memory_sections.size(), 4);
}

TEST(topology_m150_every_io_resolves) {
    const Topology& t = m150();
    std::set<std::string_view> connectors;
    for (const Connector& c : t.board.connectors) {
        connectors.insert(c.id);
    }
    std::set<std::string_view> ids;
    for (const Io& io : t.board.ios) {
        CHECK(ids.insert(io.id).second);  // unique
        CHECK(t.io_type(io.io_type) != nullptr);
        for (const Pin& pin : io.pins) {
            CHECK(connectors.count(pin.connector) == 1);
            CHECK(pin.position > 0);
        }
    }
    for (const Io& io : t.board.ios) {
        for (const SubIo& part : io.parts) {
            CHECK(t.io(part.io) != nullptr);
        }
    }
    CHECK(t.io("INJ_PH1") != nullptr && t.io("INJ_PH1")->io_type == "INJ_PH");
    CHECK(t.io("NO_SUCH_IO") == nullptr);
}

TEST(topology_m150_every_device_channel_and_mode_resolves) {
    const Topology& t = m150();
    size_t channels = 0;
    for (const Device& d : t.board.devices) {
        CHECK(t.device_type(d.device_type) != nullptr);
        for (const DeviceGroup& g : d.groups) {
            for (const MethodRef& mode : g.modes) {
                CHECK(t.method(mode.feature, mode.method) != nullptr);
            }
            for (const DeviceChannel& ch : g.channels) {
                CHECK(t.io(ch.io) != nullptr || t.has_group(ch.io));
                CHECK(ch.index >= 0);
                channels++;
            }
        }
    }
    CHECK(channels > 287);  // an I/O may be served by more than one device (a bridge's ADC and its outputs)
    CHECK(t.device_type("tpu") != nullptr && t.device_type("tpu")->magic == 'd');
    CHECK(t.has_group("CAN1") && t.io("CAN1_HI") != nullptr && t.io("CAN1_HI")->group == "CAN1");
    CHECK(!t.has_group("") && !t.has_group("NO_SUCH_GROUP"));
}

TEST(topology_functions_are_numbered_once_and_their_arguments_exist) {
    const Topology& t = m150();
    std::set<int> indices;
    size_t functions = 0;
    for (const FeatureDef& def : t.features) {
        for (const Method& m : def.methods) {
            CHECK(m.feature == def.feature);
            for (const Function& f : m.functions) {
                functions++;
                if (f.index != kNoIndex) {
                    CHECK(indices.insert(f.index).second);
                    CHECK(f.index >= 0 && f.index <= 0xff);  // the low byte of the command word
                }
                for (const Argument& a : f.arguments) {
                    CHECK(a.type == "value" || a.type == "out");
                    CHECK(has_property(m, *feature_def(t, def.feature), a.property));
                }
                CHECK(f.code == "dev" || f.code == "usr");
                if (f.index == kNoIndex) {
                    CHECK(f.code == "usr");  // every hardware function is numbered
                }
                if (f.return_type == "property") {
                    CHECK(has_property(m, *feature_def(t, def.feature), f.return_property));
                }
            }
        }
    }
    CHECK_EQ(functions, 76);
    CHECK_EQ(indices.size(), 72);
    const Method* owner = nullptr;
    const Function* f = t.function(2, &owner);
    CHECK(f != nullptr && f->suffix == "periodic_cfg" && owner != nullptr && owner->id == "periodic");
    CHECK(t.function(kNoIndex) == nullptr);
    CHECK(t.function(0) == nullptr);
}

TEST(topology_m150_events_are_the_trigger_bits) {
    // ecu/thread.h: TRIGGER_1S is bit 0 ... TRIGGER_1MS is bit 9, slowest first
    const Topology& t = m150();
    double previous = 2.0;
    for (const Event& e : t.board.events) {
        CHECK(e.period_s < previous);
        previous = e.period_s;
    }
    CHECK(t.board.events[0].id == "1S" && t.board.events[0].index == 0);
    CHECK(t.board.events[9].id == "1MS" && t.board.events[9].index == 9 && t.board.events[9].period_s == 0.001);
}
