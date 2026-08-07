#ifndef SCOPE_VIDEO_STATS_H_
#define SCOPE_VIDEO_STATS_H_

#include "reflection/reflection.h"

#include <cstdint>
#include <string>

// What the video panel actually received and decoded.
//
// READ-ONLY, like every stats struct -- see the note in
// scope/panels/time_series/include/time_series/stats.h. Served through
// `scope.stats` and self-described through `scope.describe_stats`, neither of
// which knows this type exists.
//
// A BLACK PANEL HAS SEVERAL CAUSES AND THEY LOOK IDENTICAL: nothing published,
// published under a schema this binding skips, arriving but never syncing
// because the keyframe has not come round yet, syncing but failing to decode.
// Every one of them is a distinct field below, which is the whole reason to
// have this rather than a screenshot.

REFLECT_STRUCT(VideoPanelStats_t,
    (std::string, zenoh_key, "",
        "Zenoh Key", "Topic this panel is bound to"),
    (bool, bound, false,
        "Bound", "Whether the source accepted the binding"),

    (uint64_t, received, 0,
        "Received", "Access units the decoder was offered"),
    (uint64_t, decoded, 0,
        "Decoded", "Pictures the decoder produced"),
    (uint64_t, dropped_before_sync, 0,
        "Dropped Before Sync", "Access units discarded while waiting for a keyframe or "
                               "parameter sets. Non-zero at startup is normal; still "
                               "climbing means no sync point is arriving"),
    (uint64_t, decode_errors, 0,
        "Decode Errors", "Packets libavcodec rejected"),
    (uint64_t, convert_errors, 0,
        "Convert Errors", "Decoded frames swscale could not turn into RGB"),
    (bool, synced, false,
        "Synced", "Whether a sync point has been seen and frames are being fed through"),

    (uint64_t, buffered, 0,
        "Buffered", "Encoded access units currently held"),
    (uint64_t, bytes, 0,
        "Bytes", "Encoded video currently held"),
    (uint64_t, dropped_messages, 0,
        "Dropped Messages", "Messages the source could not hand over because the buffer "
                            "was full. Above zero means the panel is missing frames"),
    (uint64_t, keyframes, 0,
        "Keyframes", "Seek points among the buffered access units"),

    (bool, has_data, false,
        "Has Data", "False when nothing is buffered, which makes the times below "
                    "meaningless rather than zero"),
    (double, t_first, 0.0,
        "First (s)", "Time of the oldest buffered access unit, on the source's clock"),
    (double, t_last, 0.0,
        "Last (s)", "Time of the newest buffered access unit, on the source's clock"),

    (bool, has_frame, false,
        "Has Frame", "Whether a picture has been decoded and is on screen"),

    // THE LOAD-BEARING ONE. Everything else says the panel is working; this says
    // it is showing the instant it was told to. A seek that lands on the wrong
    // GOP still decodes cleanly and still draws a picture -- this is the only
    // field that catches it, and it is what makes "seek to t twice, get the same
    // frame" an assertion rather than an impression.
    (double, frame_t, 0.0,
        "Frame Time (s)", "Source-clock time of the picture currently displayed"),

    (uint64_t, frame_width, 0,
        "Frame Width", "Width of the decoded picture in pixels"),
    (uint64_t, frame_height, 0,
        "Frame Height", "Height of the decoded picture in pixels")
)

#endif  // SCOPE_VIDEO_STATS_H_
