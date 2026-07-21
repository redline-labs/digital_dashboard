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

#endif // CARPLAY_CONFIG_H
