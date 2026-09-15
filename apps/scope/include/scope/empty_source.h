#ifndef SCOPE_EMPTY_SOURCE_H_
#define SCOPE_EMPTY_SOURCE_H_

#include "scope/data_source.h"

namespace scope
{

// Offline with nothing loaded: the source a window holds before you point it at
// anything.
//
// It exists because "no source" cannot be expressed as a null pointer here.
// Every panel, the browser and the time base hold a DataSource& and dereference
// it on a render tick, so a null would have to be checked at every one of those
// call sites -- and the check would be forgotten in exactly the paths that only
// run before the first bag is opened. An object that answers "nothing, and I am
// not live" makes the empty case the same code path as every other case.
//
// NOT live and NOT seekable, which is what makes the transport bar correct for
// free: applySourceCaps() shows the live controls on caps().live and the
// playback controls on caps().seekable, so both sets hide themselves here
// without knowing this class exists.
class EmptySource : public DataSource
{
  public:
    SourceCaps caps() const override
    {
        SourceCaps caps;
        caps.live = false;
        caps.seekable = false;
        return caps;
    }

    std::vector<TopicInfo> topics() const override { return {}; }

    // Never bumps. The browser polls this and does nothing while it is
    // unchanged, so an offline window costs it one integer compare per tick
    // rather than a rebuild of an empty tree.
    std::uint64_t topicsRevision() const override { return 0; }

    // Declines rather than accepting a binding it could never feed. A workspace
    // loaded while offline still gets its panels and its traces -- they are
    // simply unbound, which is the state the browser and `scope.sample_stats`
    // already know how to report, and which resolves itself the moment a source
    // arrives and Panel::rebindTo() runs.
    SignalHandle bind(const SignalKey& /*key*/, std::shared_ptr<SignalBuffer> /*into*/) override
    {
        return kInvalidSignal;
    }

    void release(SignalHandle /*handle*/) override {}

    double now() const override { return 0.0; }
};

}  // namespace scope

#endif  // SCOPE_EMPTY_SOURCE_H_
