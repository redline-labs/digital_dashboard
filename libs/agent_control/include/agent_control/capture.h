#ifndef AGENT_CONTROL_CAPTURE_H_
#define AGENT_CONTROL_CAPTURE_H_

#include "agent_control/error.h"
#include "agent_control/locator.h"

#include <QRect>
#include <QWidget>

#include <string>

namespace agent_control
{

struct CaptureOptions
{
    // Widget-local crop. Null rect => the whole widget.
    QRect region;

    // Longest edge of the returned image. The capture happens at full
    // resolution and is scaled afterwards, so this trades tokens for detail
    // without changing what was rendered.
    int max_dim = 1024;

    // When set and equal to the hash of the freshly captured image, no image is
    // returned. Screenshots are the expensive payload in this interface; a
    // polling loop that mostly sees no change should mostly cost nothing.
    std::string if_changed_from;

    // Draw a numbered box and label over each addressable child widget before
    // encoding, and return the number->selector mapping alongside.
    //
    // Set-of-mark prompting: picking a target off a labelled picture is far more
    // reliable than correlating a bare screenshot against a separate snapshot
    // list, especially on a dashboard where several gauges look alike.
    bool annotate = false;
};

// Captures `widget` and returns a JSON object carrying the base64 PNG plus the
// metadata that makes its coordinates unambiguous:
//
//   image_png_base64, image_size [w,h], scale, dpr,
//   logical_rect [x,y,w,h]  -- the widget-local region captured
//   target, revision, hash
//
// THE CONTRACT: an agent reading pixel (px,py) in the returned image converts to
// a widget-local coordinate with (px/scale + logical_rect.x, py/scale +
// logical_rect.y), and that value is what input.click takes. No global screen
// coordinates appear anywhere.
Result<json> captureWidget(WidgetLocator& locator, QWidget* widget, const CaptureOptions& options);

}  // namespace agent_control

#endif  // AGENT_CONTROL_CAPTURE_H_
