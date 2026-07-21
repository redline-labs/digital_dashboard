#ifndef NOW_PLAYING_CONFIG_H
#define NOW_PLAYING_CONFIG_H

#include <string>
#include "helpers/color.h"
#include "reflection/reflection.h"

// Now-playing widget: renders media metadata published by the carplay driver
// node. Purely a subscriber -- it works alongside (or entirely without) the
// CarPlay video widget, which is the point of publishing metadata separately.
REFLECT_STRUCT(NowPlayingConfig_t,
    (std::string, zenoh_key, "nodes/carplay/nowplaying"),
    (bool, show_album_art, true),
    (bool, show_progress, true),
    (helpers::Color, title_color, "#FFFFFF"),
    (helpers::Color, detail_color, "#AAAAAA"),
    (helpers::Color, accent_color, "#FFA500")
)

REFLECT_METADATA(NowPlayingConfig_t,
    (zenoh_key, "Zenoh Key", "Zenoh topic publishing CarPlayNowPlaying metadata"),
    (show_album_art, "Show Album Art", "Draw album artwork when the phone provides it"),
    (show_progress, "Show Progress", "Draw the track progress bar and elapsed/duration times"),
    (title_color, "Title Color", "Color of the track title"),
    (detail_color, "Detail Color", "Color of the artist/album/app text"),
    (accent_color, "Accent Color", "Color of the progress bar")
)

#endif // NOW_PLAYING_CONFIG_H
