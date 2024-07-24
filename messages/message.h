#ifndef MESSAGE_H_
#define MESSAGE_H_

#include "audio_type.h"
#include "command_mapping.h"
#include "message_type.h"
#include "phone_type.h"

#include <array>
#include <string>

class MessageHeader
{
  public:
    constexpr static size_t kDataLength = 16u;
    constexpr static uint32_t kMagic = 0x55aa55aa;

    MessageHeader(size_t length, MessageType type);

    static MessageHeader from_buffer(const std::array<uint8_t, kDataLength>& buffer);

    MessageType get_message_type();

  private:
    size_t _length;
    MessageType _type;
};


class Message
{
  public:
    constexpr static std::string_view name = "Message";

    Message(MessageHeader header);

    MessageType get_type();

  private:
    MessageHeader _header;
};

class Command : Message
{
  public:
    constexpr static std::string_view name = "Command";

    Command(MessageHeader header, const uint8_t* buffer);

  private:
    CommandMapping _value;
};

class ManufacturerInfo : Message
{
  public:
    constexpr static std::string_view name = "ManufacturerInfo";

    ManufacturerInfo(MessageHeader header, const uint8_t* buffer);

  private:
    uint32_t _a;
    uint32_t _b;
};


class SoftwareVersion : Message
{
  public:
    constexpr static std::string_view name = "SoftwareVersion";

    SoftwareVersion(MessageHeader header, const uint8_t* buffer);

  private:
    std::string _version;
};


class BluetoothAddress : Message
{
  public:
    constexpr static std::string_view name = "BluetoothAddress";

    BluetoothAddress(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented
};


class BluetoothPIN : Message
{
  public:
    constexpr static std::string_view name = "BluetoothPIN";

    BluetoothPIN(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented
};


class BluetoothDeviceName : Message
{
  public:
    constexpr static std::string_view name = "BluetoothDeviceName";

    BluetoothDeviceName(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented
};


class WiFiDeviceName : Message
{
  public:
    constexpr static std::string_view name = "WiFiDeviceName";

    WiFiDeviceName(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented
};


class HiCarLink : Message
{
  public:
    constexpr static std::string_view name = "HiCarLink";

    HiCarLink(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented
};


class BluetoothPairedList : Message
{
  public:
    constexpr static std::string_view name = "BluetoothPairedList";

    BluetoothPairedList(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented
};


class Plugged : Message
{
  public:
    constexpr static std::string_view name = "Plugged";

    Plugged(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented
};


class Unplugged : Message
{
  public:
    constexpr static std::string_view name = "Unplugged";

    Unplugged(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented
};


class AudioData : Message
{
  public:
    constexpr static std::string_view name = "AudioData";

    AudioData(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented
};


class VideoData : Message
{
  public:
    constexpr static std::string_view name = "VideoData";

    VideoData(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented
};


class MediaData : Message
{
  public:
    constexpr static std::string_view name = "MediaData";

    MediaData(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented
};


class Opened : Message
{
  public:
    constexpr static std::string_view name = "Opened";

    Opened(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented
};


class BoxInfo : Message
{
  public:
    constexpr static std::string_view name = "BoxInfo";

    BoxInfo(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented
};


class Phase : Message
{
  public:
    constexpr static std::string_view name = "Phase";

    Phase(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented
};



#endif  // MESSAGE_H_