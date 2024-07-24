#include "message.h"

static uint32_t read_uint32_t_little_endian(const uint8_t* buffer)
{
    return
        (static_cast<uint32_t>(buffer[0]) <<  0) |
        (static_cast<uint32_t>(buffer[1]) <<  8) |
        (static_cast<uint32_t>(buffer[2]) << 16) |
        (static_cast<uint32_t>(buffer[3]) << 24);
}


MessageHeader::MessageHeader(size_t length, MessageType type) :
    _length{length},
    _type{type}
{
};


MessageHeader MessageHeader::from_buffer(const std::array<uint8_t, kDataLength>& buffer)
{
    MessageHeader ret{0u, MessageType::Invalid};

    // Verify magic number.
    uint32_t received_magic = read_uint32_t_little_endian(&buffer[0]);
    if (received_magic == kMagic)
    {
        ret._length = read_uint32_t_little_endian(&buffer[4]);
        ret._type = static_cast<MessageType>(read_uint32_t_little_endian(&buffer[8]));

        // uint32_t type_check = read_uint32_t_little_endian(&buffer[12]);
        // if (typeCheck != ((msgType ^ -1) & 0xffffffff) >>> 0) {
        // throw new HeaderBuildError(`Invalid type check, received ${typeCheck}`)
    }

    return ret;
}

MessageType MessageHeader::get_message_type()
{
    return _type;
}



Message::Message(MessageHeader header) :
  _header{header}
{
}

MessageType Message::get_type()
{
    return _header.get_message_type();
}


Command::Command(MessageHeader header, const uint8_t* buffer) :
  Message(header)
{
    _value = static_cast<CommandMapping>(read_uint32_t_little_endian(&buffer[0]));
}


ManufacturerInfo::ManufacturerInfo(MessageHeader header, const uint8_t* buffer) :
  Message(header)
{
    _a = read_uint32_t_little_endian(&buffer[0]);
    _b = read_uint32_t_little_endian(&buffer[4]);
}


SoftwareVersion::SoftwareVersion(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header),
  _version{} // buffer
{
}


BluetoothAddress::BluetoothAddress(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

BluetoothPIN::BluetoothPIN(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

BluetoothDeviceName::BluetoothDeviceName(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

WiFiDeviceName::WiFiDeviceName(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

HiCarLink::HiCarLink(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

BluetoothPairedList::BluetoothPairedList(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

Plugged::Plugged(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

Unplugged::Unplugged(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

AudioData::AudioData(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

VideoData::VideoData(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

MediaData::MediaData(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

Opened::Opened(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

BoxInfo::BoxInfo(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

Phase::Phase(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}
