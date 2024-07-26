#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

#include <string>

enum class DriveType
{
    LHD = 0u,
    RHD
};

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
        phone_config{}
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
};


app_config_t load_app_config(const std::string& config_filepath);


#endif  // APP_CONFIG_H_
