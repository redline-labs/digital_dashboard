#ifndef SCOPE_OVERVIEW_CONTROLLER_H_
#define SCOPE_OVERVIEW_CONTROLLER_H_

#include <cstdint>
#include <utility>
#include <vector>

namespace scope
{

class ScopeWindow;

// The overview strip's data side: the extent, the view region, and the
// throttled message-density histogram behind them.
//
// Split out of ScopeWindow because it is a self-contained layer with its own
// cache and its own invariants -- chiefly that the histogram is recomputed on a
// clock, never per frame, because CaptureBuffer::density() walks the retained
// deque under the mutex the zenoh RX thread needs to push. The window stays
// the orchestrator: it owns the strip widget (agent tests address it by
// objectName on the window) and calls in once per render tick.
//
// A friend of ScopeWindow rather than fed through an interface: this is the
// window's own private machinery in a separate file, not a reusable component,
// and a parameter object restating six of the window's members would be a
// second description of the same state.
class OverviewController
{
  public:
    explicit OverviewController(ScopeWindow& window) : window_(window) {}

    // Push the extent/retained/view/playhead numbers into the strip, then
    // refresh the histogram if it is due. Called once per render tick from the
    // transport update.
    void updateOverview();

    // Messages per uniform bucket over [begin, end] on the source's clock --
    // the window's densityFor() delegates here. A live source declines and the
    // recorder answers, through the wall-clock epoch reconciliation.
    bool densityFor(double begin, double end, std::size_t buckets,
                    std::vector<std::uint32_t>& out);

    // The [begin, end] the histogram is computed over. ONE function, used by
    // refreshDensity() AND scope.density, so the RPC always reports the
    // histogram the strip is drawing.
    std::pair<double, double> densityRange() const;

    // The cached histogram describes the OLD source after a swap; forcing a
    // recompute here rather than waiting for the throttle is what stops a
    // bag's shape being drawn under a live view for half a second.
    void forceRecompute() { computed_at_ms_ = 0; }

  private:
    void refreshDensity();

    ScopeWindow& window_;

    // The histogram cache. The cache keys are the bucket count (a resize
    // recomputes at once) and the clock; deliberately NOT the range -- on a
    // live source the range's end is now() and never compares equal, which
    // silently defeated the throttle. See refreshDensity().
    std::vector<std::uint32_t> density_;
    int buckets_ = 0;
    std::int64_t computed_at_ms_ = 0;
};

}  // namespace scope

#endif  // SCOPE_OVERVIEW_CONTROLLER_H_
