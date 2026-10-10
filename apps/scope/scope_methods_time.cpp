#include "scope_methods_detail.h"

namespace scope
{
namespace methods_detail
{

// The shared clock: the view window, and the density histogram behind
// the overview strip.
void registerTimeMethods(const FlushedRegistrar& registerFlushed, ScopeWindow& window)
{
    ScopeWindow* const win = &window;


    // -------------------------------------------------------------- time base

    registerFlushed(
        "scope.time_base",
        [win](const json& params) -> MethodResult {
            TimeBase& time_base = win->timeBase();

            // Everything is read and checked before anything is applied: a
            // request refused for its fifth parameter must not have applied its
            // first four.
            const auto window_seconds = optionalParam<double>(params, "window_seconds");
            const auto mode = optionalParam<std::string>(params, "mode");
            const auto render_rate = optionalParam<int>(params, "render_rate_hz");
            const auto rate = optionalParam<double>(params, "rate");
            const auto playing = optionalParam<bool>(params, "playing");
            const auto seek = optionalParam<double>(params, "seek");
            const auto pan = optionalParam<double>(params, "pan");
            const auto fit = optionalParam<bool>(params, "fit");
            const auto following = optionalParam<bool>(params, "following");
            if (auto error = firstError(window_seconds, mode, render_rate, rate, playing, seek, pan,
                                        fit, following))
            {
                return std::unexpected(*error);
            }

            // `mode` is APPLIED after the view movers, beside `following` -- it
            // is the same flag, and a pan clears it. Applying it first would
            // make {"mode":"live","pan":-10} depend on the order the handlers
            // happen to be written in.
            std::optional<bool> want_following;
            if (*mode == "live")
            {
                want_following = true;
            }
            else if (*mode == "paused")
            {
                want_following = false;
            }
            else if (mode->has_value())
            {
                return std::unexpected(
                    badParams("'mode' must be 'live' or 'paused', not '" + **mode + "'."));
            }

            // `view` is a pair and `zoom` a factor or {factor, anchor}, so they
            // are read by hand -- but here, with the rest.
            const json* const view = params.contains("view") && !params["view"].is_null()
                                         ? &params["view"]
                                         : nullptr;
            if (view != nullptr && (!view->is_array() || view->size() != 2 ||
                                    !(*view)[0].is_number() || !(*view)[1].is_number()))
            {
                return std::unexpected(badParams("'view' must be [begin, end], both numbers."));
            }

            // A bare number is the factor; an object carries an anchor. The
            // anchor is what a wheel gesture has and a keyboard shortcut does
            // not, so both shapes are worth accepting.
            std::optional<double> zoom_factor;
            std::optional<double> zoom_anchor;
            if (params.contains("zoom") && !params["zoom"].is_null())
            {
                const json& zoom = params["zoom"];
                if (zoom.is_number())
                {
                    zoom_factor = zoom.get<double>();
                }
                else if (zoom.is_object())
                {
                    const auto factor = requireParam<double>(zoom, "factor");
                    const auto anchor = optionalParam<double>(zoom, "anchor");
                    if (auto error = firstError(factor, anchor))
                    {
                        AgentError wrapped = *error;
                        wrapped.message = "zoom: " + wrapped.message;
                        return std::unexpected(std::move(wrapped));
                    }
                    zoom_factor = *factor;
                    zoom_anchor = *anchor;
                }
                else
                {
                    return std::unexpected(badParams(
                        "'zoom' must be a factor, or {factor, anchor}. Below 1 zooms in."));
                }
                if (!(*zoom_factor > 0.0))
                {
                    return std::unexpected(badParams("'zoom' factor must be greater than zero."));
                }
            }

            // Null clears the cursor, so for this one null is not "absent".
            const bool cursor_given = params.is_object() && params.contains("cursor");
            if (cursor_given && !params["cursor"].is_null() && !params["cursor"].is_number())
            {
                return std::unexpected(badParams("'cursor' must be a number or null."));
            }

            // AT MOST ONE mover, and the check is not pedantry. They all move
            // the window, so composing two silently produces a result nobody can
            // explain from the request -- and the caller is usually a model that
            // will then reason from the wrong position. `fit: false` asks for
            // nothing, so it is not one.
            const int movers = int(seek->has_value()) + int(view != nullptr) + int(pan->has_value()) +
                               int(zoom_factor.has_value()) + int(fit->value_or(false));
            if (movers > 1)
            {
                return std::unexpected(
                    badParams("seek, view, pan, zoom and fit all move the view; name one."));
            }
            if (seek->has_value() && !time_base.source().caps().seekable)
            {
                return std::unexpected(badParams(
                    "This source is not seekable. Open a recording with "
                    "scope.open_recording first."));
            }

            // ------------------------------------------------------ applying

            if (window_seconds->has_value())
            {
                time_base.setWindowSeconds(**window_seconds);
            }
            if (render_rate->has_value())
            {
                time_base.setRenderRateHz(**render_rate);
            }

            // Playback. All three are no-ops on a live source, which has
            // nothing to seek to -- so a caller that did not read caps() first
            // gets an unchanged reply rather than an error, and the reply says
            // why.
            if (rate->has_value())
            {
                time_base.setRate(**rate);
            }
            // BEFORE the seek, so {"playing": true, "seek": 0} starts from the
            // sought position rather than from wherever the head already was.
            if (playing->has_value())
            {
                time_base.setPlaying(**playing);
            }

            if (seek->has_value())
            {
                time_base.seek(**seek);
            }
            if (view != nullptr)
            {
                time_base.setView((*view)[0].get<double>(), (*view)[1].get<double>());
            }
            if (pan->has_value())
            {
                time_base.panBy(**pan);
            }
            if (zoom_factor)
            {
                time_base.zoomAt(
                    zoom_anchor.value_or((time_base.viewBegin() + time_base.viewEnd()) / 2.0),
                    *zoom_factor);
            }
            if (fit->value_or(false))
            {
                time_base.fitAll();
            }

            // AFTER the movers, so {"pan": -10, "following": true} resolves to
            // the explicit flag rather than to the pan's side effect.
            if (following->has_value())
            {
                want_following = **following;
            }
            if (want_following)
            {
                time_base.setFollowing(*want_following);
            }

            if (cursor_given)
            {
                const json& cursor = params["cursor"];
                time_base.setCursor(cursor.is_null() ? std::nullopt
                                                     : std::optional<double>(cursor.get<double>()));
            }

            // Any mover above only PARKED its seek (they coalesce to the
            // render tick); apply it now so this response describes the state
            // the caller just asked for. Without this, {"seek": 10}'s own
            // reply showed the position from before the seek -- correct on the
            // NEXT call thanks to the dispatcher's flush, but confusing for an
            // agent reading the reply it is holding.
            time_base.flushSeek();

            const SourceCaps caps = time_base.source().caps();
            json out;
            out["window_seconds"] = time_base.windowSeconds();
            out["render_rate_hz"] = time_base.renderRateHz();
            out["mode"] = time_base.mode() == TimeBase::Mode::Live ? "live" : "paused";
            out["view_begin"] = time_base.viewBegin();
            out["view_end"] = time_base.viewEnd();
            out["now"] = time_base.source().now();
            if (time_base.cursor())
            {
                out["cursor"] = *time_base.cursor();
            }
            else
            {
                out["cursor"] = nullptr;
            }
            out["playing"] = time_base.playing();
            out["rate"] = time_base.rate();

            // `mode` stays forever -- it is what every existing caller sends and
            // reads. `following` is the same flag under its real name.
            out["following"] = time_base.following();

            // What the view will be CLAMPED to, so a caller can see the bound it
            // is working inside rather than discovering it by being clamped. On a
            // live source this is narrower than caps: a bus has no beginning, and
            // what bounds the view is how far back the buffers still reach.
            const auto [available_begin, available_end] = time_base.availableRange();
            out["available_begin"] = available_begin;
            out["available_end"] = available_end;
            out["history_seconds"] = time_base.retentionSeconds();

            // t_begin/t_end are only meaningful when seekable, and are reported
            // regardless so a caller can see the extent it is allowed to seek
            // within rather than discovering it by being clamped.
            out["caps"] = json{{"live", caps.live},
                               {"seekable", caps.seekable},
                               {"t_begin", caps.t_begin},
                               {"t_end", caps.t_end}};
            return out;
        },
        agent_control::AgentServer::MethodKind::kMutating);

    // ----------------------------------------------------------------- density

    // What the overview strip draws behind everything else, as numbers.
    //
    // This exists because a screenshot cannot say whether the strip's background
    // is the real shape of the recording or a plausible-looking artefact -- the
    // same reason sample_stats is the thing to reach for before a picture. The
    // bucket sum against scope.capture's `messages` is the assertion worth
    // making.
    registerFlushed("scope.density", [win](const json& params) -> MethodResult {
        const auto requested = optionalParam<std::size_t>(params, "buckets");
        if (!requested)
        {
            return std::unexpected(requested.error());
        }
        const std::size_t buckets = std::min<std::size_t>(requested->value_or(200), 4096);
        if (buckets == 0)
        {
            return std::unexpected(badParams("'buckets' must be at least 1."));
        }

        // The strip's own range, from the same function refreshDensity() uses.
        // NOT availableRange(): that is deliberately unfloored, so for the
        // first history_seconds of a live session the two disagreed and this
        // method reported a histogram the strip was not drawing.
        const auto [begin, end] = win->densityRange();

        // Through the window, NOT straight to the source: a live source cannot
        // answer and the recorder can, and this method exists to check what the
        // overview strip is drawing. Asking the source directly would report
        // "nothing" under a strip visibly full of data.
        std::vector<std::uint32_t> counts;
        const bool exact = win->densityFor(begin, end, buckets, counts);

        json out;
        out["t_begin"] = begin;
        out["t_end"] = end;
        out["buckets"] = counts;

        // False means the source declined to answer cheaply, not that there is
        // nothing there. A bag answers from its part index, which knows HOW MANY
        // but not WHERE inside a part -- so a single-part recording is one flat
        // block and says so rather than implying detail it does not have.
        out["exact"] = exact;
        return out;
    });
}

}  // namespace methods_detail
}  // namespace scope
