#ifndef CARPLAY_CONFIG_H
#define CARPLAY_CONFIG_H

#include <string>
#include "reflection/reflection.h"

// Configuration for the CarPlay widget. The widget is a thin client of the
// carplay driver node (nodes/carplay), which owns the USB/iAP2/AirPlay
// session with the phone. These keys must match the driver's configuration.
REFLECT_STRUCT(CarplayConfig_t,
    (std::string, video_key,   "nodes/carplay/video",
        "Video Key", "Zenoh key the driver publishes the phone's H.264/H.265 screen on"),
    (std::string, audio_key,   "nodes/carplay/audio",
        "Audio Key", "Zenoh key the driver publishes phone audio on"),
    (std::string, mic_key,     "nodes/carplay/mic",
        "Microphone Key", "Zenoh key this widget publishes captured microphone audio on, for Siri and calls"),
    (std::string, input_key,   "nodes/carplay/input",
        "Input Key", "Zenoh key this widget publishes touch events to, to send them to the phone"),
    (std::string, session_key, "nodes/carplay/session",
        "Session Key", "Zenoh key carrying session state: whether a phone is connected and what it is doing")
)

#endif // CARPLAY_CONFIG_H
