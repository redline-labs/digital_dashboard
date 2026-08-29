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

            if (const auto seconds = params.find("window_seconds");
                seconds != params.end() && seconds->is_number())
            {
                time_base.setWindowSeconds(seconds->get<double>());
            }

            // `mode` is validated here but APPLIED after the view movers, beside
            // `following` -- it is the same flag, and a pan clears it. Applying
            // it here would make {"mode":"live","pan":-10} depend on the order
            // the handlers happen to be written in.
            std::optional<bool> want_following;
            if (const auto mode = params.find("mode"); mode != params.end() && mode->is_string())
            {
                const std::string text = mode->get<std::string>();
                if (text == "live")
                {
                    want_following = true;
                }
                else if (text == "paused")
                {
                    want_following = false;
                }
                else
                {
                    return std::unexpected(
                        badParams("'mode' must be 'live' or 'paused', not '" + text + "'."));
                }
            }

            if (const auto rate = params.find("render_rate_hz");
                rate != params.end() && rate->is_number())
            {
                time_base.setRenderRateHz(rate->get<int>());
            }

            // Playback. All three are no-ops on a live source, which has
            // nothing to seek to -- so a caller that did not read caps() first
            // gets an unchanged reply rather than an error, and the reply says
            // why.
            if (const auto rate = params.find("rate"); rate != params.end() && rate->is_number())
            {
                time_base.setRate(rate->get<double>());
            }

            // BEFORE the seek, so {"playing": true, "seek": 0} starts from the
            // sought position rather than from wherever the head already was.
            if (const auto playing = params.find("playing");
                playing != params.end() && playing->is_boolean())
            {
                time_base.setPlaying(playing->get<bool>());
            }

            // ---------------------------------------------------- the view
            //
            // AT MOST ONE of these, and the check is not pedantry. They all move
            // the window, so composing two silently produces a result nobody can
            // explain from the request -- and the caller is usually a model that
            // will then reason from the wrong position.
            {
                int movers = 0;
                for (const char* name : {"seek", "view", "pan", "zoom", "fit"})
                {
                    if (params.contains(name))
                    {
                        ++movers;
                    }
                }
                if (movers > 1)
                {
                    return std::unexpected(badParams(
                        "seek, view, pan, zoom and fit all move the view; name one."));
                }
            }

            if (const auto seek = params.find("seek"); seek != params.end() && seek->is_number())
            {
                if (!time_base.source().caps().seekable)
                {
                    return std::unexpected(badParams(
                        "This source is not seekable. Open a recording with "
                        "scope.open_recording first."));
                }
                time_base.seek(seek->get<double>());
            }

            if (const auto view = params.find("view"); view != params.end())
            {
                if (!view->is_array() || view->size() != 2 || !(*view)[0].is_number() ||
                    !(*view)[1].is_number())
                {
                    return std::unexpected(
                        badParams("'view' must be [begin, end], both numbers."));
                }
                time_base.setView((*view)[0].get<double>(), (*view)[1].get<double>());
            }

            if (const auto pan = params.find("pan"); pan != params.end() && pan->is_number())
            {
                time_base.panBy(pan->get<double>());
            }

            if (const auto zoom = params.find("zoom"); zoom != params.end())
            {
                // A bare number is the factor; an object carries an anchor. The
                // anchor is what a wheel gesture has and a keyboard shortcut does
                // not, so both shapes are worth accepting.
                double factor = 0.0;
                double anchor = (time_base.viewBegin() + time_base.viewEnd()) / 2.0;

                if (zoom->is_number())
                {
                    factor = zoom->get<double>();
                }
                else if (zoom->is_object() && zoom->contains("factor") &&
                         (*zoom)["factor"].is_number())
                {
                    factor = (*zoom)["factor"].get<double>();
                    if (zoom->contains("anchor") && (*zoom)["anchor"].is_number())
                    {
                        anchor = (*zoom)["anchor"].get<double>();
                    }
                }
                else
                {
                    return std::unexpected(badParams(
                        "'zoom' must be a factor, or {factor, anchor}. Below 1 zooms in."));
                }

                if (!(factor > 0.0))
                {
                    return std::unexpected(
                        badParams("'zoom' factor must be greater than zero."));
                }
                time_base.zoomAt(anchor, factor);
            }

            if (const auto fit = params.find("fit");
                fit != params.end() && fit->is_boolean() && fit->get<bool>())
            {
                time_base.fitAll();
            }

            // AFTER the movers, so {"pan": -10, "following": true} resolves to
            // the explicit flag rather than to the pan's side effect.
            if (const auto following = params.find("following");
                following != params.end() && following->is_boolean())
            {
                want_following = following->get<bool>();
            }
            if (want_following)
            {
                time_base.setFollowing(*want_following);
            }

            if (const auto cursor = params.find("cursor"); cursor != params.end())
            {
                if (cursor->is_null())
                {
                    time_base.setCursor(std::nullopt);
                }
                else if (cursor->is_number())
                {
                    time_base.setCursor(cursor->get<double>());
                }
                else
                {
                    return std::unexpected(
                        badParams("'cursor' must be a number or null."));
                }
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
        std::size_t buckets = 200;
        if (const auto requested = params.find("buckets");
            requested != params.end() && requested->is_number_unsigned())
        {
            buckets = std::min<std::size_t>(requested->get<std::size_t>(), 4096);
        }
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
