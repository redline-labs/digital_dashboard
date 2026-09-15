#include "scope_methods_detail.h"

namespace scope
{
namespace methods_detail
{

// What is behind the panels: online/offline, the session capture, and
// recordings on disk.
void registerSourceMethods(const FlushedRegistrar& registerFlushed, ScopeWindow& window)
{
    ScopeWindow* const win = &window;


    // ------------------------------------------------------------------ source

    registerFlushed("scope.source", [win](const json& /*params*/) -> MethodResult {
        const SourceCaps caps = win->source().caps();

        json out;

        // The MODE is the thing a caller usually wants, and it is one bit:
        // online exactly when the source tails the bus. `kind` says which of
        // the two OFFLINE sources it is, because "recorded" and "nothing
        // loaded" are answered identically by caps() -- both are non-live and
        // the empty one is not seekable either -- and a caller that could not
        // tell them apart would read an empty window as a bag full of silence.
        out["mode"] = win->isOnline() ? "online" : "offline";
        out["kind"] = caps.live ? "live" : (caps.seekable ? "recorded" : "empty");
        out["caps"] = json{{"live", caps.live},
                           {"seekable", caps.seekable},
                           {"t_begin", caps.t_begin},
                           {"t_end", caps.t_end}};
        out["now"] = win->source().now();

        // A recording is decoded once per signal, on a background thread, when
        // the signal is bound. Until this is zero a trace may legitimately be
        // empty -- so a caller that screenshots or reads sample_stats before
        // then is looking at an unfinished picture, not a broken one.
        if (const auto* recorded = dynamic_cast<const RecordedSource*>(&win->source()))
        {
            out["decodes_pending"] = recorded->decodesPending();
            out["wall_clock_ns"] = recorded->wallClockNanosAt(win->source().now());
        }

        json topics = json::array();
        for (const TopicInfo& topic : win->source().topics())
        {
            topics.push_back(json{{"key", topic.key},
                                  {"schema", topic.schema},
                                  {"reachable", topic.reachable}});
        }
        out["topics"] = std::move(topics);
        return out;
    });

    registerFlushed(
        "scope.open_recording",
        [win](const json& params) -> MethodResult {
            const auto path = params.find("path");
            if (path == params.end() || !path->is_string())
            {
                return std::unexpected(
                    badParams("'path' (string) is required -- a bag DIRECTORY, not an .mcap "
                              "file."));
            }

            if (!win->openRecording(QString::fromStdString(path->get<std::string>())))
            {
                return std::unexpected(badParams(
                    "Could not open '" + path->get<std::string>() +
                    "' as a recording. See app.logs; `bag reindex` can rebuild a missing "
                    "index."));
            }

            const SourceCaps caps = win->source().caps();
            return json{{"opened", true},
                        {"path", path->get<std::string>()},
                        {"caps", json{{"live", caps.live},
                                      {"seekable", caps.seekable},
                                      {"t_begin", caps.t_begin},
                                      {"t_end", caps.t_end}}}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    // Replaced `scope.go_live`, and the old name is GONE rather than aliased.
    //
    // An alias would have kept working while meaning something subtly different:
    // go_live used to be "stop reviewing", with a capture that ran regardless,
    // and it now has to be "attach to the bus and start capturing". A caller
    // that still said go_live would get the new behaviour under the old name --
    // which is the failure that looks like a broken app rather than a renamed
    // one, and is exactly what the transport_scrubber rename avoided.
    registerFlushed(
        "scope.set_mode",
        [win](const json& params) -> MethodResult {
            const auto mode = params.find("mode");
            if (mode == params.end() || !mode->is_string())
            {
                return std::unexpected(
                    badParams("'mode' (string) is required: 'online' or 'offline'."));
            }

            const std::string text = mode->get<std::string>();
            if (text == "online")
            {
                if (!win->goOnline())
                {
                    return std::unexpected(internalError(
                        "Refused to go online: the previous capture is unsaved and the "
                        "prompt was declined. Save it with scope.save_recording first."));
                }
            }
            else if (text == "offline")
            {
                win->goOffline();
            }
            else
            {
                return std::unexpected(
                    badParams("'mode' must be 'online' or 'offline', not '" + text + "'."));
            }

            const SourceCaps caps = win->source().caps();
            return json{{"mode", win->isOnline() ? "online" : "offline"},
                        {"caps", json{{"live", caps.live},
                                      {"seekable", caps.seekable},
                                      {"t_begin", caps.t_begin},
                                      {"t_end", caps.t_end}}}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    // ----------------------------------------------------------------- capture

    registerFlushed("scope.capture", [win](const json& /*params*/) -> MethodResult {
        ScopeRecorder* const recorder = win->recorder();

        // Not an error. A window that has never been online has no recorder at
        // all, and "there is no capture" is the honest answer to the question --
        // an error here would make the normal startup state look like a fault.
        if (recorder == nullptr)
        {
            return json{{"running", false},
                        {"messages", 0},
                        {"bytes", 0},
                        {"received", 0},
                        {"retained_span_seconds", 0.0},
                        {"evicted", 0},
                        {"evicted_bytes", 0}};
        }

        const CaptureBuffer& buffer = recorder->buffer();
        return json{
            // False once the window goes offline: the capture is a snapshot of
            // the online session, not a tail that keeps running behind it.
            {"running", recorder->isValid()},
            {"messages", buffer.size()},
            {"bytes", buffer.bytes()},
            {"received", recorder->received()},
            {"retained_span_seconds", buffer.retainedSpanSeconds()},
            // Not cosmetic. A capture silently dropping its head makes the
            // start of a trace look like a publisher that had not started yet
            // -- the same class of lie a recorder dropping samples tells.
            {"evicted", buffer.evicted()},
            {"evicted_bytes", buffer.evictedBytes()}};
    });

    registerFlushed(
        "scope.review_capture",
        [win](const json& /*params*/) -> MethodResult {
            if (!win->reviewCapture())
            {
                return std::unexpected(badParams(
                    "Nothing has been captured. Go online with scope.set_mode first, or "
                    "load a bag with scope.open_recording."));
            }
            const SourceCaps caps = win->source().caps();
            return json{{"reviewing", true},
                        {"caps", json{{"live", caps.live},
                                      {"seekable", caps.seekable},
                                      {"t_begin", caps.t_begin},
                                      {"t_end", caps.t_end}}}};
        },
        agent_control::AgentServer::MethodKind::kMutating);

    registerFlushed(
        "scope.save_recording",
        [win](const json& params) -> MethodResult {
            const auto path = params.find("path");
            if (path == params.end() || !path->is_string())
            {
                return std::unexpected(
                    badParams("'path' (string) is required -- the bag DIRECTORY to write."));
            }

            if (!win->saveCaptureTo(QString::fromStdString(path->get<std::string>())))
            {
                return std::unexpected(internalError(
                    "Failed to write the capture to '" + path->get<std::string>() + "'."));
            }

            ScopeRecorder* const recorder = win->recorder();
            return json{{"saved", true},
                        {"path", path->get<std::string>()},
                        {"messages", recorder != nullptr ? recorder->buffer().size() : 0}};
        },
        agent_control::AgentServer::MethodKind::kMutating);
}

}  // namespace methods_detail
}  // namespace scope
