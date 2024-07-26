#ifndef MESSAGE_H_
#define MESSAGE_H_

#include "audio_type.h"
#include "command_mapping.h"
#include "dongle_config_file.h"
#include "message_type.h"
#include "phone_type.h"

#include "app_config.h"

#include <array>
#include <string>
#include <vector>

class MessageHeader
{
  public:
    constexpr static size_t kDataLength = 16u;
    constexpr static uint32_t kMagic = 0x55aa55aa;

    MessageHeader(size_t length, MessageType type);

    static MessageHeader from_buffer(const std::array<uint8_t, kDataLength>& buffer);

    MessageType get_message_type();
    uint32_t get_message_type_check();

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

    std::vector<uint8_t> serialize();

  private:
    MessageHeader _header;

    virtual uint16_t get_payload_size() = 0;
    virtual void write_payload(uint8_t* buffer) = 0;
};

class Command : public Message
{
  public:
    constexpr static std::string_view name = "Command";

    Command(const uint8_t* buffer);

  private:
    CommandMapping _value;

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};

class ManufacturerInfo : Message
{
  public:
    constexpr static std::string_view name = "ManufacturerInfo";

    ManufacturerInfo(MessageHeader header, const uint8_t* buffer);

  private:
    uint32_t _a;
    uint32_t _b;

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class SoftwareVersion : Message
{
  public:
    constexpr static std::string_view name = "SoftwareVersion";

    SoftwareVersion(MessageHeader header, const uint8_t* buffer);

  private:
    std::string _version;

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class BluetoothAddress : Message
{
  public:
    constexpr static std::string_view name = "BluetoothAddress";

    BluetoothAddress(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class BluetoothPIN : Message
{
  public:
    constexpr static std::string_view name = "BluetoothPIN";

    BluetoothPIN(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class BluetoothDeviceName : Message
{
  public:
    constexpr static std::string_view name = "BluetoothDeviceName";

    BluetoothDeviceName(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class WiFiDeviceName : Message
{
  public:
    constexpr static std::string_view name = "WiFiDeviceName";

    WiFiDeviceName(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class HiCarLink : Message
{
  public:
    constexpr static std::string_view name = "HiCarLink";

    HiCarLink(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class BluetoothPairedList : Message
{
  public:
    constexpr static std::string_view name = "BluetoothPairedList";

    BluetoothPairedList(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class Plugged : Message
{
  public:
    constexpr static std::string_view name = "Plugged";

    Plugged(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class Unplugged : Message
{
  public:
    constexpr static std::string_view name = "Unplugged";

    Unplugged(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class AudioData : Message
{
  public:
    constexpr static std::string_view name = "AudioData";

    AudioData(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class VideoData : Message
{
  public:
    constexpr static std::string_view name = "VideoData";

    VideoData(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class MediaData : Message
{
  public:
    constexpr static std::string_view name = "MediaData";

    MediaData(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class Opened : Message
{
  public:
    constexpr static std::string_view name = "Opened";

    Opened(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class BoxInfo : Message
{
  public:
    constexpr static std::string_view name = "BoxInfo";

    BoxInfo(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class Phase : Message
{
  public:
    constexpr static std::string_view name = "Phase";

    Phase(MessageHeader header, const uint8_t* buffer);

  private:
    // Unimplemented

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};



class Heartbeat : public Message
{
  public:
    constexpr static std::string_view name = "Heartbeat";

    Heartbeat(const uint8_t* buffer);

  private:
    CommandMapping _value;

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class SendOpen : public Message
{
  public:
    constexpr static std::string_view name = "SendOpen";
    constexpr static uint16_t kPayloadBytes = 28;

    SendOpen(const app_config_t& config);

  private:
    app_config_t _config;

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class SendFile : public Message
{
  public:
    constexpr static std::string_view name = "SendFile";

    SendFile(DongleConfigFile file, const std::vector<uint8_t>& buffer);

  private:
    DongleConfigFile _file;
    std::vector<uint8_t> _buffer;

    uint16_t get_payload_size() final;
    void write_payload(uint8_t* buffer) final;
};


class SendBoolean : public SendFile
{
  public:
    constexpr static std::string_view name = "SendBoolean";

    SendBoolean(DongleConfigFile file, bool value);
};

class SendNumber : public SendFile
{
  public:
    constexpr static std::string_view name = "SendNumber";

    SendNumber(DongleConfigFile file, uint32_t value);
};

class SendString : public SendFile
{
  public:
    constexpr static std::string_view name = "SendString";

    SendString(DongleConfigFile file, std::string value);
};

#endif  // MESSAGE_H_