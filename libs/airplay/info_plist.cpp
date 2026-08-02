// SPDX-License-Identifier: GPL-3.0-or-later

#include "airplay/info_plist.h"

#include "airplay/hid.h"
#include "airplay/oem_button.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <vector>

namespace airplay
{

plist::Value buildInfoPlist(const ReceiverConfig& config)
{
    // Field names and values follow LIVI's getInfo.ts. Note the display entry
    // uses widthPixels/heightPixels and carries the *stream type* it belongs
    // to -- there is no "width"/"height"/"refreshRate" here, and getting that
    // wrong makes the phone accept the session and then tear it down without
    // ever asking for a stream.
    constexpr int64_t kStreamTypeMainScreen = 110;
    constexpr int64_t kDisplayFeatureKnobs = 0x02;
    constexpr int64_t kDisplayFeatureHighFidelityTouch = 0x08;
    constexpr int64_t kCarplayFeatures = 0x615653aee2LL;
    constexpr int64_t kCarplayAudioFeatures = 0x10004540a00LL;

    const int64_t width_physical = 200;
    const int64_t height_physical = std::max<int64_t>(
        1, (width_physical * config.height) / std::max<uint32_t>(1, config.width));

    plist::Value display = plist::Value::dict();
    display.set("uuid", plist::Value::string(kMainDisplayUuid));
    display.set("type", plist::Value::integer(kStreamTypeMainScreen));
    display.set("maxFPS", plist::Value::integer(config.fps));
    display.set("widthPixels", plist::Value::integer(config.width));
    display.set("heightPixels", plist::Value::integer(config.height));
    display.set("widthPhysical", plist::Value::integer(width_physical));
    display.set("heightPhysical", plist::Value::integer(height_physical));
    display.set("features",
                plist::Value::integer(kDisplayFeatureHighFidelityTouch | kDisplayFeatureKnobs));
    display.set("primaryInputDevice",
                plist::Value::integer(static_cast<int64_t>(config.primary_input)));

    // The drawable region, and inside it the region safe from occlusion. LIVI
    // always sends these, and we advertise "viewAreas" in enabledFeatures --
    // claiming the feature and then supplying no areas leaves the phone unable
    // to lay anything out. Zero insets means the whole panel is usable.
    plist::Value safe_area = plist::Value::dict();
    safe_area.set("widthPixels", plist::Value::integer(config.width));
    safe_area.set("heightPixels", plist::Value::integer(config.height));
    safe_area.set("originXPixels", plist::Value::integer(0));
    safe_area.set("originYPixels", plist::Value::integer(0));
    safe_area.set("drawUIOutsideSafeArea", plist::Value::boolean(true));

    plist::Value view_area = plist::Value::dict();
    view_area.set("widthPixels", plist::Value::integer(config.width));
    view_area.set("heightPixels", plist::Value::integer(config.height));
    view_area.set("originXPixels", plist::Value::integer(0));
    view_area.set("originYPixels", plist::Value::integer(0));
    view_area.set("safeArea", std::move(safe_area));

    display.set("viewAreas", plist::Value::array({std::move(view_area)}));
    display.set("initialViewArea", plist::Value::integer(0));

    // Resource arbitration: we are willing to hand the screen and audio over at
    // any time, at a low priority.
    const auto resource = [](int64_t id) {
        constexpr int64_t kTransferTake = 1;
        constexpr int64_t kPriorityNiceToHave = 100;
        constexpr int64_t kConstraintAnytime = 100;
        plist::Value value = plist::Value::dict();
        value.set("resourceID", plist::Value::integer(id));
        value.set("transferType", plist::Value::integer(kTransferTake));
        value.set("transferPriority", plist::Value::integer(kPriorityNiceToHave));
        value.set("takeConstraint", plist::Value::integer(kConstraintAnytime));
        value.set("borrowConstraint", plist::Value::integer(kConstraintAnytime));
        value.set("unborrowConstraint", plist::Value::integer(kConstraintAnytime));
        return value;
    };

    plist::Value app_state_speech = plist::Value::dict();
    app_state_speech.set("appStateID", plist::Value::integer(1));
    app_state_speech.set("speechMode", plist::Value::integer(-1));
    plist::Value app_state_phone = plist::Value::dict();
    app_state_phone.set("appStateID", plist::Value::integer(2));
    app_state_phone.set("state", plist::Value::boolean(false));
    plist::Value app_state_nav = plist::Value::dict();
    app_state_nav.set("appStateID", plist::Value::integer(3));
    app_state_nav.set("state", plist::Value::boolean(false));

    plist::Value modes = plist::Value::dict();
    modes.set("resources", plist::Value::array({resource(1), resource(2)}));
    modes.set("appStates", plist::Value::array({std::move(app_state_phone),
                                                std::move(app_state_speech),
                                                std::move(app_state_nav)}));

    plist::Value info = plist::Value::dict();
    // Audio capability, as LIVI advertises it. Omitting this (and masking the
    // audio feature bits) is only correct behind an explicit disable-audio
    // option; a CarPlay head unit is expected to carry audio, and a phone that
    // sees none may decline to open any stream at all.
    const auto audio_latency = [](int64_t type, const char* audio_type) {
        plist::Value value = plist::Value::dict();
        value.set("type", plist::Value::integer(type));
        value.set("inputLatencyMicros", plist::Value::integer(0));
        value.set("outputLatencyMicros", plist::Value::integer(0));
        if (audio_type != nullptr)
        {
            value.set("audioType", plist::Value::string(audio_type));
        }
        return value;
    };
    const auto audio_format = [](int64_t type, const char* audio_type, int64_t output_formats,
                                 int64_t input_formats) {
        plist::Value value = plist::Value::dict();
        value.set("type", plist::Value::integer(type));
        value.set("audioType", plist::Value::string(audio_type));
        value.set("audioOutputFormats", plist::Value::integer(output_formats));
        if (input_formats != 0)
        {
            value.set("audioInputFormats", plist::Value::integer(input_formats));
        }
        return value;
    };

    // PCM voice rates plus 44.1k media, mono and stereo (LIVI's PCM constants).
    constexpr int64_t kPcmVoice = 0x3FC;
    constexpr int64_t kPcm = kPcmVoice | 0xC00;
    constexpr int64_t kPcmMono = 0x154 | 0x400;
    // AAC-LC 44.1 kHz stereo for the buffered entertainment stream (type 102).
    constexpr int64_t kAacLcMedia = 0x400000;

    info.set("audioLatencies", plist::Value::array({audio_latency(100, nullptr),
                                                    audio_latency(100, "default"),
                                                    audio_latency(100, "media"),
                                                    audio_latency(100, "telephony"),
                                                    audio_latency(100, "speechRecognition"),
                                                    audio_latency(100, "alert"),
                                                    audio_latency(101, nullptr),
                                                    audio_latency(101, "default"),
                                                    audio_latency(102, "default")}));
    // The wired phone routes all music through type 100 as PCM and works
    // cleanly; type 102 AAC-LC is advertised as a fallback for any phone that
    // does send it (verified on hardware not to be used on this wired path --
    // see docs/carplay_bringup.md stage 9).
    info.set("audioFormats",
             plist::Value::array({audio_format(100, "compatibility", kPcm, kPcmMono),
                                  audio_format(101, "compatibility", kPcm, 0),
                                  audio_format(100, "default", kPcm, kPcmMono),
                                  audio_format(100, "alert", kPcm, 0),
                                  audio_format(100, "media", kPcm, 0),
                                  audio_format(100, "telephony", kPcmMono, kPcmMono),
                                  audio_format(100, "speechRecognition", kPcmMono, kPcmMono),
                                  audio_format(101, "default", kPcm, 0),
                                  // Entertainment stream: AAC-LC, decoded by
                                  // libavcodec (aac_decoder.cpp).
                                  audio_format(102, "media", kAacLcMedia, 0)}));

    info.set("sourceVersion", plist::Value::string("366.0"));
    info.set("features", plist::Value::integer(kCarplayFeatures));
    (void)kCarplayAudioFeatures;
    info.set("statusFlags", plist::Value::integer(4));
    info.set("model", plist::Value::string(config.model));
    info.set("manufacturer", plist::Value::string("Dashboard"));
    info.set("deviceID", plist::Value::string(config.device_id));
    info.set("bluetoothIDs", plist::Value::array({plist::Value::string(config.device_id)}));
    info.set("name", plist::Value::string(config.name));
    info.set("rightHandDrive", plist::Value::boolean(false));
    info.set("keepAliveLowPower", plist::Value::boolean(true));
    info.set("keepAliveSendStatsAsBody", plist::Value::boolean(false));
    info.set("modes", std::move(modes));
    info.set("extendedFeatures",
             plist::Value::array({plist::Value::string("vocoderInfo"),
                                  plist::Value::string("enhancedRequestCarUI")}));
    info.set("displays", plist::Value::array({std::move(display)}));

    // Every input the head unit has, each its own HID device. The knob, media
    // and telephony devices cost nothing when the vehicle has no such controls
    // -- the phone simply never sees a report on them -- and advertising them
    // is the only way a vehicle that does have them can use them at all.
    info.set("hidDevices",
             plist::Value::array({hid::touchDevice(config.width, config.height, kMainDisplayUuid),
                                  hid::knobDevice(kMainDisplayUuid),
                                  hid::mediaDevice(kMainDisplayUuid),
                                  hid::telephonyDevice(kMainDisplayUuid)}));

    addOemButtonInfo(config.oem_button, info);
    if (config.oem_button.enabled)
    {
        SPDLOG_INFO("[airplay] advertising manufacturer button: label '{}', {} icon(s)",
                    config.oem_button.label, config.oem_button.icons.size());
    }
    return info;
}

}  // namespace airplay
