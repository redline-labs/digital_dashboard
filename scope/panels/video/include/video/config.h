#ifndef SCOPE_VIDEO_CONFIG_H_
#define SCOPE_VIDEO_CONFIG_H_

#include "reflection/reflection.h"

#include <string>
#include <vector>

REFLECT_STRUCT(VideoPanelConfig_t,
    (std::string, title, "Video",
        "Title", "Shown on the panel's title bar"),
    (std::string, zenoh_key, "",
        "Zenoh Key", "Topic carrying the encoded video stream. Empty until a stream is bound"),

    // ITS OWN RETENTION, not the workspace's history_seconds.
    //
    // That setting defaults to 300 s, which is right for telemetry -- five
    // minutes of a 1 kHz signal is 300k samples, a few megabytes. Five minutes
    // of CarPlay video at 4 Mbit is 150 MB of compressed payload per panel, and
    // the byte bound below would be doing all the work while the time bound sat
    // there meaning nothing. Sixty seconds is enough to scrub back over what
    // just happened, which is what a live video panel is for.
    //
    // Only the LIVE case uses this. Over a recording the panel holds one
    // seek-point window at a time and the recording itself is the history.
    (double, retention_seconds, 60.0,
        "Retention (s)", "Seconds of encoded video buffered when following a live bus"),

    // BOTH BOUNDS APPLY, whichever binds first -- the same argument
    // scope/include/scope/capture_buffer.h:19-24 makes from measurement. A
    // single keyframe can be four megabytes, so this must stay well clear of one
    // frame or the buffer thrashes.
    (uint64_t, max_buffer_bytes, 268435456,
        "Buffer Limit (bytes)", "Encoded video held in memory; 0 disables the byte bound"),

    (bool, show_scrubber, true,
        "Show Scrubber", "Draw the panel's own seek bar along its bottom edge")
)

// Clamps rather than rejects, matching the dashboard widgets' ADL validate()
// hook. Returns a note per field it had to move, which panel_registry.cpp logs
// BEFORE construction -- so the panel never sees a config the loader would have
// changed, and a saved workspace reports the clamped values rather than the
// ones it was given.
inline std::vector<std::string> validate(VideoPanelConfig_t& config)
{
    std::vector<std::string> notes;

    if (config.retention_seconds < 1.0)
    {
        notes.push_back("retention_seconds below 1 s cannot hold a single GOP; clamped to 1 s");
        config.retention_seconds = 1.0;
    }

    // Not merely "small": below one frame the buffer evicts what it was just
    // given on every push, so the panel shows nothing and looks like a dead
    // publisher rather than a misconfigured limit.
    constexpr std::uint64_t kMinUsefulBytes = 4ull * 1024 * 1024;
    if (config.max_buffer_bytes != 0 && config.max_buffer_bytes < kMinUsefulBytes)
    {
        notes.push_back("max_buffer_bytes below 4 MiB cannot hold one keyframe; clamped");
        config.max_buffer_bytes = kMinUsefulBytes;
    }

    return notes;
}

#endif  // SCOPE_VIDEO_CONFIG_H_
