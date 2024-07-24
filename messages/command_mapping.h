#ifndef COMMAND_MAPPING_H_
#define COMMAND_MAPPING_H_

#include <string_view>

enum class CommandMapping
{
    Invalid = 0, //'invalid',
    StartRecordAudio = 1,
    StopRecordAudio = 2,
    RequestHostUI = 3, //'Carplay Interface My Car button clicked',
    Siri = 5, //'Siri Button',
    Mic = 7, //'Car Microphone',
    BoxMic = 15, //'Box Microphone',
    EnableNightMode = 16, // night mode
    DisableNightMode = 17, // disable night mode
    Wifi24g = 24, //'2.4G Wifi',
    Wifi5g = 25, //'5G Wifi',
    Left = 100, //'Button Left',
    Right = 101, //'Button Right',
    Frame = 12,
    AudioTransferOn = 22, // Phone will Stream audio directly to car system and not dongle
    AudioTransferOff = 23, // DEFAULT - Phone will stream audio to the dongle and it will send it over the link
    SelectDown = 104, //'Button Select Down',
    SelectUp = 105, //'Button Select Up',
    Back = 106, //'Button Back',
    Down = 114, //'Button Down',
    Home = 200, //'Button Home',
    Play = 201, //'Button Play',
    Pause = 202, //'Button Pause',
    Next = 204, //'Button Next Track',
    Prev = 205, //'Button Prev Track',
    RequestVideoFocus = 500,
    ReleaseVideoFocus = 501,
    WifiEnable = 1000,
    AutoConnetEnable = 1001,
    WifiConnect = 1002,
    ScanningDevice = 1003,
    DeviceFound = 1004,
    DeviceNotFound = 1005,
    ConnectDeviceFailed = 1006,
    BtConnected = 1007,
    BtDisconnected = 1008,
    WifiConnected = 1009,
    WifiDisconnected = 1010,
    BtPairStart = 1011,
    WifiPair = 1012,
};

constexpr std::string_view command_mapping_to_string(CommandMapping cmd)
{
    switch (cmd)
    {
        case (CommandMapping::StartRecordAudio):
            return "StartRecordAudio";

        case (CommandMapping::StopRecordAudio):
            return "StopRecordAudio";

        case (CommandMapping::RequestHostUI):
            return "RequestHostUI";

        case (CommandMapping::Siri):
            return "Siri";

        case (CommandMapping::Mic):
            return "Mic";

        case (CommandMapping::BoxMic):
            return "BoxMic";

        case (CommandMapping::EnableNightMode):
            return "EnableNightMode";

        case (CommandMapping::DisableNightMode):
            return "DisableNightMode";

        case (CommandMapping::Wifi24g):
            return "Wifi24g";

        case (CommandMapping::Wifi5g):
            return "Wifi5g";

        case (CommandMapping::Left):
            return "Left";

        case (CommandMapping::Right):
            return "Right";

        case (CommandMapping::Frame):
            return "Frame";

        case (CommandMapping::AudioTransferOn):
            return "AudioTransferOn";

        case (CommandMapping::AudioTransferOff):
            return "AudioTransferOff";

        case (CommandMapping::SelectDown):
            return "SelectDown";

        case (CommandMapping::SelectUp):
            return "SelectUp";

        case (CommandMapping::Back):
            return "Back";

        case (CommandMapping::Down):
            return "Down";

        case (CommandMapping::Home):
            return "Home";

        case (CommandMapping::Play):
            return "Play";

        case (CommandMapping::Pause):
            return "Pause";

        case (CommandMapping::Next):
            return "Next";

        case (CommandMapping::Prev):
            return "Prev";

        case (CommandMapping::RequestVideoFocus):
            return "RequestVideoFocus";

        case (CommandMapping::ReleaseVideoFocus):
            return "ReleaseVideoFocus";

        case (CommandMapping::WifiEnable):
            return "WifiEnable";

        case (CommandMapping::AutoConnetEnable):
            return "AutoConnetEnable";

        case (CommandMapping::WifiConnect):
            return "WifiConnect";

        case (CommandMapping::ScanningDevice):
            return "ScanningDevice";

        case (CommandMapping::DeviceFound):
            return "DeviceFound";

        case (CommandMapping::DeviceNotFound):
            return "DeviceNotFound";

        case (CommandMapping::ConnectDeviceFailed):
            return "ConnectDeviceFailed";

        case (CommandMapping::BtConnected):
            return "BtConnected";

        case (CommandMapping::BtDisconnected):
            return "BtDisconnected";

        case (CommandMapping::WifiConnected):
            return "WifiConnected";

        case (CommandMapping::WifiDisconnected):
            return "WifiDisconnected";

        case (CommandMapping::BtPairStart):
            return "BtPairStart";

        case (CommandMapping::WifiPair):
            return "WifiPair";

        case (CommandMapping::Invalid):
        default:
            return "Invalid";
    }
}


#endif  // COMMAND_MAPPING_H_