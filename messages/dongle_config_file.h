#ifndef DONGLE_CONFIG_FILE_H_
#define DONGLE_CONFIG_FILE_H_


enum class DongleConfigFile
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

constexpr std::string_view get_filepath_for_dongle_config(DongleConfigFile cfg)
{
    switch (cfg)
    {
        case(DongleConfigFile::DPI):
            return "/tmp/screen_dpi";

        case(DongleConfigFile::NightMode):
            return "/tmp/night_mode";

        case(DongleConfigFile::HandDriveMode):
            return "/tmp/hand_drive_mode";

        case(DongleConfigFile::ChargeMode):
            return "/tmp/charge_mode";

        case(DongleConfigFile::BoxName):
            return "/etc/box_name";

        case(DongleConfigFile::OEMIcon):
            return "/etc/oem_icon.png";

        case(DongleConfigFile::AirplayConfig):
            return "/etc/airplay.conf";

        case(DongleConfigFile::Icon120):
            return "/etc/icon_120x120.png";

        case(DongleConfigFile::Icon180):
            return "/etc/icon_180x180.png";

        case(DongleConfigFile::Icon250):
            return "/etc/icon_256x256.png";

        case(DongleConfigFile::AndroidWorkMode):
            return "/etc/android_work_mode";

        default:
            return "";
    }
}

#endif  // DONGLE_CONFIG_H_