#ifndef SCOPE_TIME_AXIS_H_
#define SCOPE_TIME_AXIS_H_

namespace scope
{

// A linear map between a span of time and a span of pixels.
//
// It exists because the same three lines of arithmetic were written out at five
// places in the time-series panel -- the grid ticks, the traces, the cursor, the
// hover readout, and now the pan/zoom handlers -- and the moment the view can be
// zoomed they all have to agree about the same window or the cursor lands
// somewhere other than where the mouse is. The overview strip needs the same map
// over a different rect, so it is a value type with no Qt dependency rather than
// a member function of the panel.
//
// DEGENERATE SPANS ARE THE WHOLE REASON FOR THE GUARDS. A panel is briefly one
// pixel wide while a dock is being dragged, and TimeBase can be asked for a
// window before its source has produced anything. Without the guards toT()
// divides by zero, the NaN goes into TimeBase::zoomAt(), and from there into
// every other panel's view -- one degenerate widget silently corrupting the
// window's shared clock. Returning the low end is wrong but bounded, and the
// callers treat "no useful span" as a no-op.
struct TimeAxis
{
    double t0 = 0.0;
    double t1 = 1.0;
    double x0 = 0.0;
    double x1 = 1.0;

    double timeSpan() const { return t1 - t0; }
    double pixelSpan() const { return x1 - x0; }

    bool usable() const { return timeSpan() > 0.0 && pixelSpan() > 0.0; }

    double toX(double t) const
    {
        if (!usable())
        {
            return x0;
        }
        return x0 + (t - t0) / timeSpan() * pixelSpan();
    }

    double toT(double x) const
    {
        if (!usable())
        {
            return t0;
        }
        return t0 + (x - x0) / pixelSpan() * timeSpan();
    }

    // Pixels per second and its inverse, for converting a drag delta without
    // going through two absolute conversions and losing the origin.
    double secondsPerPixel() const
    {
        if (!usable())
        {
            return 0.0;
        }
        return timeSpan() / pixelSpan();
    }

    bool contains(double t) const { return t >= t0 && t <= t1; }

    // Clamped to the axis, for drawing something that may be just off the edge.
    double toClampedX(double t) const
    {
        const double x = toX(t);
        return x < x0 ? x0 : (x > x1 ? x1 : x);
    }
};

}  // namespace scope

#endif  // SCOPE_TIME_AXIS_H_
