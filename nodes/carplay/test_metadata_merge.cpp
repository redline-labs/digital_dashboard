// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the dashboard is told about navigation and the track: cleared fields
// clear, and a new song does not wear the last one's cover.
#include "metadata_merge.h"

#include <cstdio>
#include <string>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

}  // namespace

int main()
{
    // Navigation is mapped whole, so what the session cleared is cleared here.
    {
        iap2::NavGuidance session;
        iap2::RouteGuidance route;
        route.state = 1;
        route.current_road_name = "Hauptstrasse";
        route.destination_name = "Home";
        route.current_maneuver_list = std::vector<uint8_t>{0, 3};
        iap2::RouteManeuver turn;
        turn.index = 3;
        turn.maneuver_type = 4;
        turn.after_maneuver_road_name = "Bahnhofstrasse";
        session.apply(turn);
        session.apply(route);
        const auto driving = carplay::navFromSession(session);
        expect(driving.active && driving.road_name == "Hauptstrasse" && driving.maneuver_type == 4 &&
                   driving.after_road_name == "Bahnhofstrasse",
               "an active route reaches the dashboard");

        iap2::RouteGuidance ended;
        ended.state = 0;
        session.apply(ended);
        const auto parked = carplay::navFromSession(session);
        expect(!parked.active && parked.road_name.empty() && parked.destination_name.empty() &&
                   parked.maneuver_type == 0 && parked.after_road_name.empty(),
               "an ended route leaves nothing on the dashboard");
    }

    // A new title drops the cover; the same title keeps it.
    {
        carplay::NowPlaying np;
        iap2::NowPlaying first;
        first.title = "One";
        carplay::mergeNowPlaying(np, first);
        np.album_art = {0xFF, 0xD8};
        np.album_art_seq = 5;

        iap2::NowPlaying tick;
        tick.elapsed_ms = 12000;
        carplay::mergeNowPlaying(np, tick);
        expect(!np.album_art.empty() && np.album_art_seq == 5, "a tick keeps the artwork");

        iap2::NowPlaying same;
        same.title = "One";
        carplay::mergeNowPlaying(np, same);
        expect(!np.album_art.empty(), "the same title again keeps it");

        iap2::NowPlaying next;
        next.title = "Two";
        carplay::mergeNowPlaying(np, next);
        expect(np.title == "Two" && np.album_art.empty(), "a new track drops the old cover");
        expect(np.album_art_seq == 6, "and says so, so the widget clears the image");
    }

    // The phone's status, as partial updates build it.
    {
        carplay::PhoneStatus phone;
        iap2::PowerState power;
        power.battery_charge_level = 65535;
        power.battery_charging_state = 1;
        carplay::mergePower(phone, power);
        expect(phone.battery_percent && *phone.battery_percent > 99.9f, "a full battery is 100 percent");
        expect(phone.charging == true, "state 1 is charging");

        iap2::PowerState half;
        half.battery_charge_level = 32768;
        carplay::mergePower(phone, half);
        expect(phone.battery_percent && *phone.battery_percent > 49.9f && *phone.battery_percent < 50.1f,
               "half the range is half the battery");
        expect(phone.charging == true, "and an update without a charging state keeps the last one");

        iap2::PowerState charged;
        charged.battery_charging_state = 2;
        carplay::mergePower(phone, charged);
        expect(phone.charging == false, "charged is not charging");

        iap2::CellularState cell;
        cell.signal_strength = 3;
        cell.carrier_name = "Telekom";
        carplay::mergeCommunications(phone, cell);
        iap2::CellularState bars_only;
        bars_only.signal_strength = 1;
        carplay::mergeCommunications(phone, bars_only);
        expect(phone.signal_bars == 1 && phone.carrier_name == "Telekom",
               "a signal update keeps the carrier it did not mention");
        expect(!phone.airplane_mode, "and nothing the phone has not reported is invented");
    }

    if (failures == 0)
    {
        std::printf("all metadata merge checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
