#ifndef SCOPE_DATA_SOURCE_H_
#define SCOPE_DATA_SOURCE_H_

#include "scope/sample_ring.h"

#include "pub_sub/schema_registry.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace scope
{

// Opaque handle for a bound signal. Zero is never valid, so a default-
// constructed handle is detectably unset rather than accidentally naming the
// first signal.
using SignalHandle = std::uint64_t;
inline constexpr SignalHandle kInvalidSignal = 0;

// What identifies a plottable signal: the tree's existing binding form.
//
// The same triple a dashboard widget uses, deliberately. A signal picked in
// scope's browser is the same thing a gauge binds, so a workspace and a
// dashboard config describe signals identically, and the degenerate expression
// -- just the field name -- is the "plot this field" case.
struct SignalKey
{
    std::string zenoh_key;
    pub_sub::schema_type_t schema_type{};
    std::string value_expression;

    friend bool operator==(const SignalKey& lhs, const SignalKey& rhs)
    {
        return lhs.zenoh_key == rhs.zenoh_key && lhs.schema_type == rhs.schema_type &&
               lhs.value_expression == rhs.value_expression;
    }
};

// A topic this source can offer.
struct TopicInfo
{
    std::string key;
    std::string schema;

    // False once whoever offers it became unreachable. Such a topic is still
    // listed -- a picker must not evict a row the user may have bound -- but it
    // is shown as stale.
    bool reachable = true;
};

// What a source can do, which is what the transport bar renders from.
struct SourceCaps
{
    // Tails a bus: the right edge of the plot is "now" and keeps moving.
    bool live = true;

    // Supports seek(), so the view can be scrubbed. False for a live source,
    // which has nothing to seek to.
    bool seekable = false;

    // The extent of the available data. Only meaningful when seekable.
    double t_begin = 0.0;
    double t_end = 0.0;
};

// Where samples come from.
//
// This interface exists so that reading recorded data later is a new
// implementation rather than a change to every panel. The two are genuinely
// the same shape -- the same zenoh messages, the same capnp schemas, the same
// expressions over their fields -- and differ only in where the bytes and the
// timestamps come from and whether you can seek.
//
// Nothing above this interface knows which kind it has. A panel asks the time
// base what time it is; the time base asks the source what it is capable of.
class DataSource
{
  public:
    virtual ~DataSource() = default;

    virtual SourceCaps caps() const = 0;

    // What is available to plot, right now. Cheap and non-blocking: a caller
    // polls it.
    //
    // A live source answers from advertisements, so a topic is listed as soon
    // as its publisher exists -- it does not have to have published anything.
    // A recorded source would answer from its index. Neither needs a window,
    // which is why this does not take one: an interface that said "listen for
    // N milliseconds" would be describing one implementation's mechanism
    // rather than the question being asked.
    virtual std::vector<TopicInfo> topics() const = 0;

    // Bumped whenever topics() would return something different. A consumer
    // polling on a timer compares this first and does nothing when it has not
    // moved.
    virtual std::uint64_t topicsRevision() const = 0;

    // Start delivering this signal's samples into `into`.
    //
    // Returns kInvalidSignal when the binding could not be made -- an
    // expression that does not compile, a field that is not numeric, a
    // subscription that could not be declared. The caller gets a definite no
    // rather than a handle that never produces anything.
    //
    // The buffer is held by shared_ptr rather than raw pointer on purpose.
    // release() cannot join an in-flight callback without tearing down the
    // whole subscription, so a callback may still be running after a signal is
    // unbound; sharing ownership means the worst that happens is a few samples
    // written into a buffer nobody reads any more, instead of a write through a
    // dangling pointer.
    virtual SignalHandle bind(const SignalKey& key, std::shared_ptr<SignalBuffer> into) = 0;

    virtual void release(SignalHandle handle) = 0;

    // The source's current time, in seconds on its own epoch. For a live source
    // this is wall-clock-ish and always advancing; for a recorded one it is the
    // playback position.
    virtual double now() const = 0;

    // Seekable sources only; a no-op elsewhere, so a caller that does not check
    // caps() first gets nothing rather than an error.
    virtual void seek(double /*t*/) {}
    virtual void setPlaying(bool /*playing*/) {}
};

}  // namespace scope

#endif  // SCOPE_DATA_SOURCE_H_
