#include "m150/board/topology/topology.h"

namespace sim::m150::topology {

std::string_view feature_name(Feature feature) {
    switch (feature) {
        case Feature::Din:
            return "din";
        case Feature::Dout:
            return "dout";
        case Feature::Ain:
            return "ain";
        case Feature::Cfg:
            return "cfg";
        case Feature::Core:
            return "core";
    }
    return "?";
}

const Io* Topology::io(std::string_view id) const {
    for (const Io& candidate : board.ios) {
        if (candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

bool Topology::has_group(std::string_view id) const {
    if (id.empty()) {
        return false;
    }
    for (const Io& candidate : board.ios) {
        if (candidate.group == id) {
            return true;
        }
    }
    return false;
}

const IoType* Topology::io_type(std::string_view id) const {
    for (const IoType& candidate : io_types) {
        if (candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

const DeviceType* Topology::device_type(std::string_view id) const {
    for (const DeviceType& candidate : device_types) {
        if (candidate.id == id) {
            return &candidate;
        }
    }
    return nullptr;
}

const Method* Topology::method(Feature feature, std::string_view id) const {
    for (const FeatureDef& def : features) {
        if (def.feature != feature) {
            continue;
        }
        for (const Method& candidate : def.methods) {
            if (candidate.id == id) {
                return &candidate;
            }
        }
    }
    return nullptr;
}

const Function* Topology::function(int index, const Method** owner) const {
    if (index == kNoIndex) {
        return nullptr;
    }
    for (const FeatureDef& def : features) {
        for (const Method& m : def.methods) {
            for (const Function& f : m.functions) {
                if (f.index == index) {
                    if (owner != nullptr) {
                        *owner = &m;
                    }
                    return &f;
                }
            }
        }
    }
    return nullptr;
}

std::string_view Topology::characteristic(std::string_view id) const {
    for (const Characteristic& c : board.characteristics) {
        if (c.id == id) {
            return c.value;
        }
    }
    return {};
}

}  // namespace sim::m150::topology
