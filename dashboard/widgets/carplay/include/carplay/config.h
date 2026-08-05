#ifndef CARPLAY_CONFIG_H
#define CARPLAY_CONFIG_H

#include <string>
#include "reflection/reflection.h"

// Configuration for the CarPlay widget. The widget is a thin client of the
// carplay driver node (nodes/carplay), which owns the USB/iAP2/AirPlay
// session with the phone. These keys must match the driver's configuration.
REFLECT_STRUCT(CarplayConfig_t,
    (std::string, video_key,   "nodes/carplay/video"),
    (std::string, audio_key,   "nodes/carplay/audio"),
    (std::string, mic_key,     "nodes/carplay/mic"),
    (std::string, input_key,   "nodes/carplay/input"),
    (std::string, session_key, "nodes/carplay/session")
)

REFLECT_METADATA(CarplayConfig_t,
    (video_key, "Video Key", "Zenoh key the driver publishes the phone's H.264/H.265 screen on"),
    (audio_key, "Audio Key", "Zenoh key the driver publishes phone audio on"),
    (mic_key, "Microphone Key", "Zenoh key this widget publishes captured microphone audio on, for Siri and calls"),
    (input_key, "Input Key", "Zenoh key this widget publishes touch events to, to send them to the phone"),
    (session_key, "Session Key", "Zenoh key carrying session state: whether a phone is connected and what it is doing")
)

#endif // CARPLAY_CONFIG_H
