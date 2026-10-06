// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CARPLAY_METADATA_MERGE_H_
#define CARPLAY_METADATA_MERGE_H_

#include "zenoh_bridge.h"

#include "iap2/messages.h"

namespace carplay
{

// The dashboard's navigation state, from the session's merged one. Every field,
// every time: iap2::NavGuidance already holds the merge, and clears what a
// finished route or an undescribed maneuver no longer has -- assigning only
// what is present would leave those on the dashboard.
inline NavGuidance navFromSession(const iap2::NavGuidance& g)
{
    NavGuidance nav;
    // A non-zero route-guidance state means guidance is active.
    nav.active = g.status.value_or(0) != 0;
    nav.road_name = g.road_name.value_or("");
    nav.after_road_name = g.after_road_name.value_or("");
    nav.destination_name = g.destination_name.value_or("");
    nav.maneuver_type = g.maneuver_type.value_or(0);
    nav.maneuver_angle_deg = g.turn_angle.value_or(0);
    nav.junction_type = g.junction_type.value_or(0);
    // iap2::NavGuidance names these confusingly:
    //   distance_to_destination = total distance remaining
    //   remain_distance         = distance to the next maneuver
    nav.distance_remaining_m = static_cast<float>(g.distance_to_destination.value_or(0));
    nav.distance_to_maneuver_m = static_cast<float>(g.remain_distance.value_or(0));
    nav.time_remaining_sec = static_cast<float>(g.time_to_destination.value_or(0));
    nav.eta_epoch_sec = g.eta_epoch.value_or(0);
    return nav;
}

// Folds one partial now-playing update into the accumulated state: a track
// change carries title, artist and album, a tick may carry only the elapsed
// time. A new title drops the artwork -- the next track's arrives afterwards,
// as a file transfer, and until then the old cover would sit on the new song.
inline void mergeNowPlaying(NowPlaying& np, const iap2::NowPlaying& update)
{
    if (update.title)
    {
        if (*update.title != np.title && !np.album_art.empty())
        {
            np.album_art.clear();
            ++np.album_art_seq;
        }
        np.title = *update.title;
    }
    if (update.artist)
    {
        np.artist = *update.artist;
    }
    if (update.album)
    {
        np.album = *update.album;
    }
    if (update.app_name)
    {
        np.app = *update.app_name;
    }
    if (update.duration_ms)
    {
        np.duration_sec = static_cast<float>(*update.duration_ms) / 1000.0f;
    }
    if (update.elapsed_ms)
    {
        np.elapsed_sec = static_cast<float>(*update.elapsed_ms) / 1000.0f;
    }
    if (update.status)
    {
        np.playing = (*update.status == iap2::PlaybackStatus::kPlaying);
    }
}

}  // namespace carplay

#endif  // CARPLAY_METADATA_MERGE_H_
