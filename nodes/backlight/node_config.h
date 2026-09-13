// Which display's backlight this node looks after, and how.
//
// One display per process: a second panel gets a second node with
// `role: secondary`, the same way a second CAN bus gets a second channel rather
// than a second bridge.
#ifndef BACKLIGHT_NODE_CONFIG_H
#define BACKLIGHT_NODE_CONFIG_H

#include <cstdint>
#include <string>

namespace backlight_node
{

struct NodeConfig
{
    // The display role, which names the record file: primary or secondary.
    std::string role { "primary" };

    // Where redline-display-setup writes the records. Only worth changing to
    // point the node at a fake tree off the target.
    std::string recordDir { "/run/redline/displays" };

    // Everything this node publishes and serves hangs off here. Empty means
    // nodes/backlight/<role>.
    std::string topicPrefix;

    // How often the status topic is read and published.
    uint32_t pollMs { 500 };

    // The lowest brightness a set may apply, as a percentage of max_brightness.
    // A backlight at zero is indistinguishable from a dead panel, which is not
    // something a slider or a remote should be able to do by accident. The
    // panel's own range is 1..100 % PWM.
    double minPercent { 1.0 };

    std::string resolvedTopicPrefix() const
    {
        return topicPrefix.empty() ? "nodes/backlight/" + role : topicPrefix;
    }
};

// Reads the file. Returns false and logs every problem it found rather than the
// first, so a config with three typos takes one run to fix.
bool load_node_config(const std::string& path, NodeConfig& out);

// Exposed for tests.
bool parse_node_config(const std::string& yaml, NodeConfig& out);

} // namespace backlight_node

#endif // BACKLIGHT_NODE_CONFIG_H
