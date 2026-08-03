// SPDX-License-Identifier: GPL-3.0-or-later
//
// Routes libavcodec/libswscale logging into spdlog.
//
// By default those libraries write straight to stderr, unformatted and
// untimestamped, so their output interleaves with ours and does not survive the
// same filtering -- a `--verbose` run of the CarPlay node used to carry a wall
// of "[libx264 @ 0x...]" encoder statistics with no timestamps, sitting between
// lines that had them.
//
// Levels are mapped deliberately rather than one-to-one. ffmpeg's AV_LOG_INFO
// is where codecs put their per-run statistics, which is debug detail by our
// reckoning; warnings and errors keep their severity.
#ifndef HELPERS_FFMPEG_LOG_H_
#define HELPERS_FFMPEG_LOG_H_

namespace helpers
{

// Idempotent and safe to call from any thread, but call it once at startup
// before opening a codec.
void routeFfmpegLogsToSpdlog();

}  // namespace helpers

#endif  // HELPERS_FFMPEG_LOG_H_
