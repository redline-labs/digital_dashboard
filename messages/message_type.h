#ifndef MESSAGE_TYPE_H_
#define MESSAGE_TYPE_H_

#include <cstdint>

enum class MessageType
{
    Open = 0x01,
    Plugged = 0x02,
    Phase = 0x03,
    Unplugged = 0x04,
    Touch = 0x05,
    VideoData = 0x06,
    AudioData = 0x07,
    Command = 0x08,
    LogoType = 0x09,
    DisconnectPhone = 0xf,
    CloseDongle = 0x15,
    BluetoothAddress = 0x0a,
    BluetoothPIN = 0x0c,
    BluetoothDeviceName = 0x0d,
    WifiDeviceName = 0x0e,
    BluetoothPairedList = 0x12,
    ManufacturerInfo = 0x14,
    MultiTouch = 0x17,
    HiCarLink = 0x18,
    BoxSettings = 0x19,
    MediaData = 0x2a,
    SendFile = 0x99,
    HeartBeat = 0xaa,
    SoftwareVersion = 0xcc,

    Invalid = 0xFF
};

constexpr std::string_view msg_type_to_string(MessageType cmd)
{
    switch (cmd)
    {
        case (MessageType::Open):
            return "Open";

        case (MessageType::Plugged):
            return "Plugged";

        case (MessageType::Phase):
            return "Phase";

        case (MessageType::Unplugged):
            return "Unplugged";

        case (MessageType::Touch):
            return "Touch";

        case (MessageType::VideoData):
            return "VideoData";

        case (MessageType::AudioData):
            return "AudioData";

        case (MessageType::Command):
            return "Command";

        case (MessageType::LogoType):
            return "LogoType";

        case (MessageType::DisconnectPhone):
            return "DisconnectPhone";

        case (MessageType::CloseDongle):
            return "CloseDongle";

        case (MessageType::BluetoothAddress):
            return "BluetoothAddress";

        case (MessageType::BluetoothPIN):
            return "BluetoothPIN";

        case (MessageType::BluetoothDeviceName):
            return "BluetoothDeviceName";

        case (MessageType::WifiDeviceName):
            return "WifiDeviceName";

        case (MessageType::BluetoothPairedList):
            return "BluetoothPairedList";

        case (MessageType::ManufacturerInfo):
            return "ManufacturerInfo";

        case (MessageType::MultiTouch):
            return "MultiTouch";

        case (MessageType::HiCarLink):
            return "HiCarLink";

        case (MessageType::BoxSettings):
            return "BoxSettings";

        case (MessageType::MediaData):
            return "MediaData";

        case (MessageType::SendFile):
            return "SendFile";

        case (MessageType::HeartBeat):
            return "HeartBeat";

        case (MessageType::SoftwareVersion):
            return "SoftwareVersion";

        case (MessageType::Invalid):
        default:
            return "Invalid";
    }
}

#endif  // MESSAGE_TYPE_H_