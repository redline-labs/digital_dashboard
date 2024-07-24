#ifndef DONGLE_CONFIG_H_
#define DONGLE_CONFIG_H_


enum class DongleConfig
{
    DPI,
    NightMode,
    HandDriveMode,
    ChargeMode,
    BoxName,
    OEMIcon,
    AirplayConfig,
    Icon120,
    Icon180,
    Icon250,
    AndroidWorkMode
};

constexpr std::string_view get_filepath_for_dongle_config(DongleConfig cfg)
{
    switch (cfg)
    {
        case(DongleConfig::DPI):
            return "/tmp/screen_dpi";

        case(DongleConfig::NightMode):
            return "/tmp/night_mode";

        case(DongleConfig::HandDriveMode):
            return "/tmp/hand_drive_mode";

        case(DongleConfig::ChargeMode):
            return "/tmp/charge_mode";

        case(DongleConfig::BoxName):
            return "/etc/box_name";

        case(DongleConfig::OEMIcon):
            return "/etc/oem_icon.png";

        case(DongleConfig::AirplayConfig):
            return "/etc/airplay.conf";

        case(DongleConfig::Icon120):
            return "/etc/icon_120x120.png";

        case(DongleConfig::Icon180):
            return "/etc/icon_180x180.png";

        case(DongleConfig::Icon250):
            return "/etc/icon_256x256.png";

        case(DongleConfig::AndroidWorkMode):
            return "/etc/android_work_mode";

        default:
            return "";
    }
}

#endif  // DONGLE_CONFIG_H_