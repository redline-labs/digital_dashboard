#ifndef PUB_SUB_TOPIC_DISCOVERY_H_
#define PUB_SUB_TOPIC_DISCOVERY_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pub_sub
{

// Observing what is on the bus by subscribing for a window and recording what
// arrives. There is no registry to query: zenoh has no retained messages, so the
// only way to know a topic exists is to see a sample of it.
//
// Shared by nodes/inspect and the agent control interface so the two cannot
// disagree about what "the topics" are.

struct TopicObservation
{
    std::string key;
    std::string schema;   // From the sample's encoding; empty if not capnp-tagged.
    std::uint64_t count = 0;
    double hz = 0.0;
};

// Subscribes to `keyexpr` for `window_ms` and reports every distinct key seen,
// with its message count and rate.
//
// Anything publishing more slowly than the window is invisible to this -- an
// empty result means "nothing published during the window", not "nothing
// exists". Callers should say so rather than reporting an empty bus.
std::vector<TopicObservation> observeTopics(const std::string& keyexpr, int window_ms);

struct SampleBytes
{
    std::string key;
    std::string schema;
    std::vector<std::uint8_t> payload;
};

// Waits up to `timeout_ms` for one sample on `keyexpr`.
std::optional<SampleBytes> readOneSample(const std::string& keyexpr, int timeout_ms);

}  // namespace pub_sub

#endif  // PUB_SUB_TOPIC_DISCOVERY_H_
