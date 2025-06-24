#ifndef CARPLAY_CONFIG_H
#define CARPLAY_CONFIG_H

#include <string>
#include <cstdint>
#include <optional>

struct carplay_config_t {
    carplay_config_t() :
        libusb_debug{false},
        audio_buffer_size{},
        video_format{"h264"}
    {}

    bool libusb_debug;                          // Enable USB debugging output
    std::optional<uint32_t> audio_buffer_size;  // Audio buffer size override
    std::string video_format;                   // Preferred video codec
};


#endif // CARPLAY_CONFIG_H