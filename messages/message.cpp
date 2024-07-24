#include "message.h"

static uint32_t read_uint32_t_little_endian(const uint8_t* buffer)
{
    return
        (static_cast<uint32_t>(buffer[0]) <<  0) |
        (static_cast<uint32_t>(buffer[1]) <<  8) |
        (static_cast<uint32_t>(buffer[2]) << 16) |
        (static_cast<uint32_t>(buffer[3]) << 24);
}

static void write_uint32_t_little_endian(uint32_t value, uint8_t* buffer)
{
    buffer[0] = (value >>  0) & 0xFF;
    buffer[1] = (value >>  8) & 0xFF;
    buffer[2] = (value >> 16) & 0xFF;
    buffer[3] = (value >> 24) & 0xFF;
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

uint32_t MessageHeader::get_message_type_check()
{
    return (static_cast<uint32_t>(_type) ^ -1) & 0xffffffff;
}


Message::Message(MessageHeader header) :
  _header{header}
{
}

MessageType Message::get_type()
{
    return _header.get_message_type();
}

std::vector<uint8_t> Message::serialize()
{
    uint16_t payload_size_bytes = get_payload_size();
    std::vector<uint8_t> ret(MessageHeader::kDataLength + payload_size_bytes);

    const uint32_t converted_msg_type = static_cast<uint32_t>(_header.get_message_type());

    write_uint32_t_little_endian(MessageHeader::kMagic,                 &ret[0]);
    write_uint32_t_little_endian(payload_size_bytes,                    &ret[4]);
    write_uint32_t_little_endian(converted_msg_type,                    &ret[8]);
    write_uint32_t_little_endian(_header.get_message_type_check(),      &ret[12]);

    write_payload(&ret[16]);

    return ret;
}

/* ---------------------------------------------------------------------------- */
/* Command                                                                      */
/* ---------------------------------------------------------------------------- */
Command::Command(const uint8_t* buffer) :
  Message({0u, MessageType::Command})
{
    _value = static_cast<CommandMapping>(read_uint32_t_little_endian(&buffer[0]));
}

uint16_t Command::get_payload_size()
{
    return 0u;
}

void Command::write_payload(uint8_t* /*buffer*/)
{
    return;
}

/* ---------------------------------------------------------------------------- */
/* ManufacturerInfo                                                             */
/* ---------------------------------------------------------------------------- */
ManufacturerInfo::ManufacturerInfo(MessageHeader header, const uint8_t* buffer) :
  Message(header)
{
    _a = read_uint32_t_little_endian(&buffer[0]);
    _b = read_uint32_t_little_endian(&buffer[4]);
}

uint16_t ManufacturerInfo::get_payload_size()
{
    return 0u;
}

void ManufacturerInfo::write_payload(uint8_t* /*buffer*/)
{
    return;
}

/* ---------------------------------------------------------------------------- */
/* SoftwareVersion                                                              */
/* ---------------------------------------------------------------------------- */
SoftwareVersion::SoftwareVersion(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header),
  _version{} // buffer
{
}

uint16_t SoftwareVersion::get_payload_size()
{
    return 0u;
}

void SoftwareVersion::write_payload(uint8_t* /*buffer*/)
{
    return;
}

/* ---------------------------------------------------------------------------- */
/* BluetoothAddress                                                             */
/* ---------------------------------------------------------------------------- */
BluetoothAddress::BluetoothAddress(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

uint16_t BluetoothAddress::get_payload_size()
{
    return 0u;
}

void BluetoothAddress::write_payload(uint8_t* /*buffer*/)
{
    return;
}

/* ---------------------------------------------------------------------------- */
/* BluetoothPIN                                                                 */
/* ---------------------------------------------------------------------------- */
BluetoothPIN::BluetoothPIN(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

uint16_t BluetoothPIN::get_payload_size()
{
    return 0u;
}

void BluetoothPIN::write_payload(uint8_t* /*buffer*/)
{
    return;
}

/* ---------------------------------------------------------------------------- */
/* BluetoothDeviceName                                                          */
/* ---------------------------------------------------------------------------- */
BluetoothDeviceName::BluetoothDeviceName(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

uint16_t BluetoothDeviceName::get_payload_size()
{
    return 0u;
}

void BluetoothDeviceName::write_payload(uint8_t* /*buffer*/)
{
    return;
}

/* ---------------------------------------------------------------------------- */
/* WiFiDeviceName                                                               */
/* ---------------------------------------------------------------------------- */
WiFiDeviceName::WiFiDeviceName(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

uint16_t WiFiDeviceName::get_payload_size()
{
    return 0u;
}

void WiFiDeviceName::write_payload(uint8_t* /*buffer*/)
{
    return;
}

/* ---------------------------------------------------------------------------- */
/* HiCarLink                                                                    */
/* ---------------------------------------------------------------------------- */
HiCarLink::HiCarLink(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

uint16_t HiCarLink::get_payload_size()
{
    return 0u;
}

void HiCarLink::write_payload(uint8_t* /*buffer*/)
{
    return;
}

/* ---------------------------------------------------------------------------- */
/* BluetoothPairedList                                                          */
/* ---------------------------------------------------------------------------- */
BluetoothPairedList::BluetoothPairedList(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

uint16_t BluetoothPairedList::get_payload_size()
{
    return 0u;
}

void BluetoothPairedList::write_payload(uint8_t* /*buffer*/)
{
    return;
}

/* ---------------------------------------------------------------------------- */
/* Plugged                                                                      */
/* ---------------------------------------------------------------------------- */
Plugged::Plugged(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

uint16_t Plugged::get_payload_size()
{
    return 0u;
}

void Plugged::write_payload(uint8_t* /*buffer*/)
{
    return;
}

/* ---------------------------------------------------------------------------- */
/* Unplugged                                                                    */
/* ---------------------------------------------------------------------------- */
Unplugged::Unplugged(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

uint16_t Unplugged::get_payload_size()
{
    return 0u;
}

void Unplugged::write_payload(uint8_t* /*buffer*/)
{
    return;
}

/* ---------------------------------------------------------------------------- */
/* AudioData                                                                    */
/* ---------------------------------------------------------------------------- */
AudioData::AudioData(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

uint16_t AudioData::get_payload_size()
{
    return 0u;
}

void AudioData::write_payload(uint8_t* /*buffer*/)
{
    return;
}

/* ---------------------------------------------------------------------------- */
/* VideoData                                                                    */
/* ---------------------------------------------------------------------------- */
VideoData::VideoData(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

uint16_t VideoData::get_payload_size()
{
    return 0u;
}

void VideoData::write_payload(uint8_t* /*buffer*/)
{
    return;
}

/* ---------------------------------------------------------------------------- */
/* MediaData                                                                    */
/* ---------------------------------------------------------------------------- */
MediaData::MediaData(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

uint16_t MediaData::get_payload_size()
{
    return 0u;
}

void MediaData::write_payload(uint8_t* /*buffer*/)
{
    return;
}

/* ---------------------------------------------------------------------------- */
/* Opened                                                                       */
/* ---------------------------------------------------------------------------- */
Opened::Opened(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

uint16_t Opened::get_payload_size()
{
    return 0u;
}

void Opened::write_payload(uint8_t* /*buffer*/)
{
    return;
}

/* ---------------------------------------------------------------------------- */
/* BoxInfo                                                                      */
/* ---------------------------------------------------------------------------- */
BoxInfo::BoxInfo(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

uint16_t BoxInfo::get_payload_size()
{
    return 0u;
}

void BoxInfo::write_payload(uint8_t* /*buffer*/)
{
    return;
}

/* ---------------------------------------------------------------------------- */
/* Phase                                                                        */
/* ---------------------------------------------------------------------------- */
Phase::Phase(MessageHeader header, const uint8_t* /*buffer*/) :
  Message(header)
{
}

uint16_t Phase::get_payload_size()
{
    return 0u;
}

void Phase::write_payload(uint8_t* /*buffer*/)
{
    return;
}



/* ---------------------------------------------------------------------------- */
/* Heartbeat                                                                    */
/* ---------------------------------------------------------------------------- */
Heartbeat::Heartbeat(const uint8_t* buffer) :
  Message({0u, MessageType::HeartBeat})
{
    _value = static_cast<CommandMapping>(read_uint32_t_little_endian(&buffer[0]));
}

uint16_t Heartbeat::get_payload_size()
{
    return 0u;
}

void Heartbeat::write_payload(uint8_t* /*buffer*/)
{
    return;
}


/* ---------------------------------------------------------------------------- */
/* SendFile                                                                     */
/* ---------------------------------------------------------------------------- */
SendFile::SendFile(DongleConfig config, const std::vector<uint8_t>& buffer) :
  Message({0u, MessageType::SendFile}),
  _config{config},
  _buffer{buffer}
{
}

uint16_t SendFile::get_payload_size()
{
    uint16_t filename_length = get_filepath_for_dongle_config(_config).size() + 1;  // Add null terminator.
    uint16_t content_length = _buffer.size();

    return 4u + filename_length + 4u + content_length;
}

void SendFile::write_payload(uint8_t* buffer)
{
    std::string_view filename_str = get_filepath_for_dongle_config(_config);
    uint16_t filename_str_size = filename_str.size() + 1u;  // Add null terminator.

    write_uint32_t_little_endian(filename_str_size, &buffer[0]);

    std::copy(filename_str.begin(), filename_str.end(), &buffer[4]);

    write_uint32_t_little_endian(_buffer.size(), &buffer[filename_str_size + 4]);

    std::copy(_buffer.begin(), _buffer.end(), &buffer[filename_str.size() + 1 + 4 + 4]);
}
