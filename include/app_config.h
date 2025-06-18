#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class DriveType
{
    LHD = 0u,
    RHD
};

static constexpr std::string_view drive_type_to_string(DriveType type)
{
    switch (type)
    {
        case (DriveType::LHD):
            return "LHD";

        case (DriveType::RHD):
            return "RHD";

        default:
            return "INVALID";
    }
}

enum class WiFiType
{
    Disabled = 0u,
    WiFi_2_4_GHz,
    WiFi_5_GHz
};

enum class MicType
{
    Box = 0u,
    OS
};

struct carplay_phone_config_t
{
    carplay_phone_config_t() : frame_interval{5000} {};

    int32_t frame_interval;
};

struct android_auto_phone_config_t
{
    android_auto_phone_config_t() : frame_interval{-1} {};
    int32_t frame_interval;
};

struct phone_config_t
{
    phone_config_t() : car_play{}, android_auto{} {};

    carplay_phone_config_t car_play;
    android_auto_phone_config_t android_auto;
};

struct widget_config_t {
    widget_config_t() :
        type{},
        x{0},
        y{0},
        width{100},
        height{100},
        zenoh_key{}
    {}

    std::string type;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    std::string zenoh_key;  // Optional Zenoh subscription key for real-time data
};

struct window_config_t {
    window_config_t() :
        name{},
        width{800},
        height{480},
        widgets{}
    {}

    std::string name;
    uint16_t width;
    uint16_t height;
    std::vector<widget_config_t> widgets;
};

struct app_config_t {

    app_config_t() :
        width_px{800},
        height_px{640},
        fps{20},
        dpi{160},
        format{5},
        i_box_version{2},
        phone_work_mode{2},
        packet_max{49152},
        box_name("nodePlay"),
        night_mode{false},
        drive_type{DriveType::LHD},
        media_delay{300},
        audio_transfer_mode{false},
        wifi_type{WiFiType::WiFi_5_GHz},
        mic_type{MicType::OS},
        phone_config{},
        audio_device_buffer_size{8192},
        windows{}
    {}

    uint16_t width_px;
    uint16_t height_px;
    uint8_t fps;
    uint16_t dpi;
    uint8_t format;
    uint8_t i_box_version;
    uint16_t phone_work_mode;
    uint32_t packet_max;
    std::string box_name;
    bool night_mode;
    DriveType drive_type;
    uint16_t media_delay;
    bool audio_transfer_mode;
    WiFiType wifi_type;
    MicType mic_type;
    phone_config_t phone_config;

    // Host settings.
    uint32_t audio_device_buffer_size;

    // Window layout configuration - support multiple windows
    std::vector<window_config_t> windows;
};


app_config_t load_app_config(const std::string& config_filepath);


#endif  // APP_CONFIG_H_
