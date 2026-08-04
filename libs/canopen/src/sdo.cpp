// SPDX-License-Identifier: GPL-3.0-or-later

#include "canopen/sdo.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

namespace canopen
{
namespace
{

// Client command specifiers, in the top three bits of byte 0.
constexpr uint8_t kCcsDownload = 0x20;         // 001
constexpr uint8_t kCcsUpload = 0x40;           // 010
constexpr uint8_t kCcsUploadSegment = 0x60;    // 011

// Server command specifiers.
constexpr uint8_t kScsUploadSegment = 0x00;    // 000
constexpr uint8_t kScsDownload = 0x60;         // 011
constexpr uint8_t kScsUpload = 0x40;           // 010
constexpr uint8_t kScsAbort = 0x80;            // 100

constexpr uint8_t kExpedited = 0x02;
constexpr uint8_t kSizeIndicated = 0x01;
constexpr uint8_t kToggle = 0x10;

uint8_t command_specifier(uint8_t byte)
{
    return byte & 0xE0;
}

helpers::CanFrame make_request(uint8_t nodeId)
{
    helpers::CanFrame frame {};
    frame.id = SdoClient::kRequestCobIdBase + nodeId;
    frame.len = 8;
    return frame;
}

void put_index(helpers::CanFrame& frame, uint16_t index, uint8_t sub)
{
    frame.data[1] = static_cast<uint8_t>(index & 0xFF);
    frame.data[2] = static_cast<uint8_t>((index >> 8) & 0xFF);
    frame.data[3] = sub;
}

uint16_t get_index(const helpers::CanFrame& frame)
{
    return static_cast<uint16_t>(frame.data[1] | (frame.data[2] << 8));
}

uint32_t get_u32(const helpers::CanFrame& frame, size_t offset)
{
    return static_cast<uint32_t>(frame.data[offset])
        | (static_cast<uint32_t>(frame.data[offset + 1]) << 8)
        | (static_cast<uint32_t>(frame.data[offset + 2]) << 16)
        | (static_cast<uint32_t>(frame.data[offset + 3]) << 24);
}

SdoError timeout_error(uint8_t nodeId, uint16_t index, uint8_t sub, Duration timeout)
{
    return SdoError { SdoError::Kind::Timeout, 0,
                      fmt::format("no SDO response from node {} for 0x{:04X}:{:02X} within {} ms",
                                  nodeId, index, sub, timeout.count()) };
}

SdoError bad_response(std::string message)
{
    return SdoError { SdoError::Kind::BadResponse, 0, std::move(message) };
}

// The expedited download command byte for a payload of 1..4 bytes: the size is
// carried in the `n` field as the number of bytes that are NOT data.
uint8_t expedited_download_command(size_t size)
{
    return static_cast<uint8_t>(kCcsDownload | kExpedited | kSizeIndicated
                                | ((4 - size) << 2));
}

} // namespace

helpers::CanFrame make_sdo_upload_frame(uint8_t nodeId, uint16_t index, uint8_t sub)
{
    helpers::CanFrame frame = make_request(nodeId);
    frame.data[0] = kCcsUpload;
    put_index(frame, index, sub);
    return frame;
}

helpers::CanFrame make_sdo_download_frame(uint8_t nodeId, uint16_t index, uint8_t sub,
                                          std::span<const uint8_t> bytes)
{
    helpers::CanFrame frame = make_request(nodeId);
    frame.data[0] = expedited_download_command(bytes.size());
    put_index(frame, index, sub);
    for (size_t i = 0; i < bytes.size() && i < 4; ++i)
    {
        frame.data[4 + i] = bytes[i];
    }
    return frame;
}

std::string format_frame_data(const helpers::CanFrame& frame)
{
    std::string out;
    for (uint8_t i = 0; i < frame.len && i < frame.data.size(); ++i)
    {
        out += fmt::format("{}{:02X}", i == 0 ? "" : " ", frame.data[i]);
    }
    return out;
}

std::string describe_abort(uint32_t code)
{
    const char* name = nullptr;
    switch (static_cast<SdoAbortCode>(code))
    {
    case SdoAbortCode::ToggleBitNotAlternated: name = "toggle bit not alternated"; break;
    case SdoAbortCode::TimedOut: name = "SDO protocol timed out"; break;
    case SdoAbortCode::CommandSpecifierInvalid: name = "command specifier not valid"; break;
    case SdoAbortCode::UnsupportedAccess: name = "unsupported access to this object"; break;
    case SdoAbortCode::ReadOfWriteOnly: name = "attempt to read a write-only object"; break;
    case SdoAbortCode::WriteOfReadOnly: name = "attempt to write a read-only object"; break;
    case SdoAbortCode::ObjectDoesNotExist: name = "object does not exist"; break;
    case SdoAbortCode::CannotBeMappedToPdo: name = "object cannot be mapped to a PDO"; break;
    case SdoAbortCode::PdoLengthExceeded: name = "mapping would exceed the PDO length"; break;
    case SdoAbortCode::GeneralParameterIncompatibility:
        name = "general parameter incompatibility";
        break;
    case SdoAbortCode::GeneralInternalIncompatibility:
        name = "general internal incompatibility";
        break;
    case SdoAbortCode::HardwareError: name = "hardware error"; break;
    case SdoAbortCode::LengthMismatch: name = "data type does not match, length mismatch"; break;
    case SdoAbortCode::LengthTooHigh: name = "data type does not match, length too high"; break;
    case SdoAbortCode::LengthTooLow: name = "data type does not match, length too low"; break;
    case SdoAbortCode::SubIndexDoesNotExist: name = "sub-index does not exist"; break;
    case SdoAbortCode::ValueRangeExceeded: name = "value outside its range"; break;
    case SdoAbortCode::ValueTooHigh: name = "value too high"; break;
    case SdoAbortCode::ValueTooLow: name = "value too low"; break;
    case SdoAbortCode::GeneralError: name = "general error"; break;
    case SdoAbortCode::CannotTransfer: name = "cannot transfer or store the data"; break;
    case SdoAbortCode::CannotTransferLocalControl:
        name = "cannot transfer: local control";
        break;
    case SdoAbortCode::CannotTransferDeviceState:
        name = "cannot transfer: device state";
        break;
    case SdoAbortCode::NoObjectDictionary: name = "no object dictionary present"; break;
    }

    if (name == nullptr)
    {
        return fmt::format("abort 0x{:08X}", code);
    }
    return fmt::format("abort 0x{:08X} ({})", code, name);
}

std::string to_string(const SdoError& error)
{
    return error.message;
}

uint64_t SdoData::as_uint() const
{
    uint64_t value = 0;
    const size_t n = std::min<size_t>(bytes.size(), 8);
    for (size_t i = 0; i < n; ++i)
    {
        value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    }
    return value;
}

SdoClient::SdoClient(Bus& bus, uint8_t nodeId, Duration timeout)
    : bus_(bus)
    , nodeId_(nodeId)
    , timeout_(timeout)
{
    bus_.subscribe(
        [this](const helpers::CanFrame& frame)
        {
            if ((frame.id & 0x7FF) != (kResponseCobIdBase + nodeId_))
            {
                return;
            }
            // Only one request is ever in flight, so the latest response is
            // the one being waited for. A second frame arriving before the
            // first is consumed replaces it, which is what a device that
            // answered twice deserves.
            pending_.frame = frame;
            pending_.present = true;
        });
}

SdoClient::~SdoClient() = default;

void SdoClient::on_exchange(std::function<void(const std::string&)> callback)
{
    exchangeCallback_ = std::move(callback);
}

void SdoClient::log(const std::string& line) const
{
    if (exchangeCallback_)
    {
        exchangeCallback_(line);
    }
}

void SdoClient::log_frame(const char* direction, const helpers::CanFrame& frame) const
{
    if (!exchangeCallback_)
    {
        return;
    }
    log(fmt::format("{} {:03X} [{}] {}", direction, frame.id, frame.len,
                    format_frame_data(frame)));
}

SdoResult<helpers::CanFrame> SdoClient::exchange(const helpers::CanFrame& request, uint16_t index,
                                                 uint8_t sub, bool checkEcho)
{
    pending_.present = false;

    log_frame("->", request);
    bus_.send(request);

    if (!wait_until(bus_, timeout_, [this] { return pending_.present; }))
    {
        return std::unexpected(timeout_error(nodeId_, index, sub, timeout_));
    }

    const helpers::CanFrame response = pending_.frame;
    pending_.present = false;
    log_frame("<-", response);

    // A CANopen SDO response is always eight bytes. A short one is a device
    // doing something the protocol does not allow, and decoding it would mean
    // reading bytes the sender never set.
    if (response.len != 8)
    {
        return std::unexpected(bad_response(fmt::format(
            "SDO response for 0x{:04X}:{:02X} has {} byte(s), expected 8", index, sub,
            response.len)));
    }

    if (command_specifier(response.data[0]) == kScsAbort)
    {
        const uint32_t code = get_u32(response, 4);
        return std::unexpected(SdoError {
            SdoError::Kind::Abort, code,
            fmt::format("node {} refused 0x{:04X}:{:02X}: {}", nodeId_, index, sub,
                        describe_abort(code)) });
    }

    // The echo check is what catches a response to a *different* request --
    // a stale frame from a previous exchange, or a device answering the wrong
    // object. Segment responses do not echo the index, hence the flag.
    if (checkEcho)
    {
        const uint16_t echoedIndex = get_index(response);
        const uint8_t echoedSub = response.data[3];
        if (echoedIndex != index || echoedSub != sub)
        {
            return std::unexpected(bad_response(
                fmt::format("asked node {} for 0x{:04X}:{:02X} but it answered about "
                            "0x{:04X}:{:02X}",
                            nodeId_, index, sub, echoedIndex, echoedSub)));
        }
    }

    return response;
}

// ============================================================================
// Upload
// ============================================================================

SdoResult<SdoData> SdoClient::upload(uint16_t index, uint8_t sub)
{
    const helpers::CanFrame request = make_sdo_upload_frame(nodeId_, index, sub);
    auto response = exchange(request, index, sub, true);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }

    const uint8_t command = response->data[0];
    if (command_specifier(command) != kScsUpload)
    {
        return std::unexpected(bad_response(
            fmt::format("upload of 0x{:04X}:{:02X} got command byte 0x{:02X}, which is not an "
                        "upload response",
                        index, sub, command)));
    }

    if ((command & kExpedited) != 0)
    {
        // The `n` field counts the bytes that are NOT data. Without the size
        // indicator the device is telling us it does not know, and CiA 301
        // says to assume all four.
        const size_t unused = (command & kSizeIndicated) != 0 ? ((command >> 2) & 0x03) : 0;
        const size_t size = 4 - unused;

        SdoData data;
        data.expedited = true;
        data.bytes.assign(response->data.begin() + 4, response->data.begin() + 4 + size);
        return data;
    }

    // Normal (segmented) transfer. The four bytes hold the total length when
    // the size indicator is set.
    const size_t declaredSize = (command & kSizeIndicated) != 0 ? get_u32(*response, 4) : 0;
    return upload_segmented(index, sub, declaredSize);
}

SdoResult<SdoData> SdoClient::upload_segmented(uint16_t index, uint8_t sub, size_t declaredSize)
{
    SdoData data;
    data.expedited = false;
    if (declaredSize != 0)
    {
        data.bytes.reserve(declaredSize);
    }

    bool toggle = false;
    // A device that never sets the "last segment" bit would otherwise spin
    // here forever. Seven bytes per segment, so this bounds a transfer at
    // roughly 7 KiB -- far more than any object on this device.
    constexpr int kMaxSegments = 1024;

    for (int segment = 0; segment < kMaxSegments; ++segment)
    {
        helpers::CanFrame request = make_request(nodeId_);
        request.data[0] = static_cast<uint8_t>(kCcsUploadSegment | (toggle ? kToggle : 0));

        // Segment responses echo neither index nor sub-index, so the echo
        // check is off and the toggle bit is what keeps the exchange in step.
        auto response = exchange(request, index, sub, false);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }

        const uint8_t command = response->data[0];
        if (command_specifier(command) != kScsUploadSegment)
        {
            return std::unexpected(bad_response(fmt::format(
                "segmented upload of 0x{:04X}:{:02X} got command byte 0x{:02X}, which is not a "
                "segment response",
                index, sub, command)));
        }
        if (((command & kToggle) != 0) != toggle)
        {
            return std::unexpected(bad_response(
                fmt::format("segmented upload of 0x{:04X}:{:02X}: toggle bit did not alternate",
                            index, sub)));
        }

        const size_t unused = (command >> 1) & 0x07;
        const size_t size = 7 - unused;
        data.bytes.insert(data.bytes.end(), response->data.begin() + 1,
                          response->data.begin() + 1 + size);

        const bool last = (command & kSizeIndicated) != 0;
        if (last)
        {
            return data;
        }

        toggle = !toggle;
    }

    return std::unexpected(bad_response(
        fmt::format("segmented upload of 0x{:04X}:{:02X} did not end after {} segments", index, sub,
                    kMaxSegments)));
}

SdoResult<uint8_t> SdoClient::upload_u8(uint16_t index, uint8_t sub)
{
    auto data = upload(index, sub);
    if (!data.has_value())
    {
        return std::unexpected(data.error());
    }
    return static_cast<uint8_t>(data->as_uint());
}

SdoResult<uint16_t> SdoClient::upload_u16(uint16_t index, uint8_t sub)
{
    auto data = upload(index, sub);
    if (!data.has_value())
    {
        return std::unexpected(data.error());
    }
    return static_cast<uint16_t>(data->as_uint());
}

SdoResult<uint32_t> SdoClient::upload_u32(uint16_t index, uint8_t sub)
{
    auto data = upload(index, sub);
    if (!data.has_value())
    {
        return std::unexpected(data.error());
    }
    return static_cast<uint32_t>(data->as_uint());
}

SdoResult<std::string> SdoClient::upload_string(uint16_t index, uint8_t sub)
{
    auto data = upload(index, sub);
    if (!data.has_value())
    {
        return std::unexpected(data.error());
    }

    std::string text(data->bytes.begin(), data->bytes.end());
    // VISIBLE_STRING is not required to be terminated, but devices commonly
    // pad with NULs to fill a segment.
    const size_t end = text.find('\0');
    if (end != std::string::npos)
    {
        text.resize(end);
    }
    return text;
}

// ============================================================================
// Download
// ============================================================================

SdoResult<void> SdoClient::download(uint16_t index, uint8_t sub, std::vector<uint8_t> bytes)
{
    if (bytes.empty() || bytes.size() > 4)
    {
        // Segmented download is not implemented: nothing in the
        // reconfiguration writes an object wider than four bytes, and an
        // untested code path that only runs against hardware is worse than an
        // honest refusal.
        return std::unexpected(bad_response(
            fmt::format("cannot write {} byte(s) to 0x{:04X}:{:02X}; expedited download carries "
                        "1 to 4",
                        bytes.size(), index, sub)));
    }

    const helpers::CanFrame request = make_sdo_download_frame(nodeId_, index, sub, bytes);
    auto response = exchange(request, index, sub, true);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }

    if (command_specifier(response->data[0]) != kScsDownload)
    {
        return std::unexpected(bad_response(
            fmt::format("write to 0x{:04X}:{:02X} got command byte 0x{:02X}, which is not a "
                        "download response",
                        index, sub, response->data[0])));
    }

    return {};
}

SdoResult<void> SdoClient::download_u8(uint16_t index, uint8_t sub, uint8_t value)
{
    return download(index, sub, { value });
}

SdoResult<void> SdoClient::download_u16(uint16_t index, uint8_t sub, uint16_t value)
{
    return download(index, sub,
                    { static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>(value >> 8) });
}

SdoResult<void> SdoClient::download_u32(uint16_t index, uint8_t sub, uint32_t value)
{
    return download(index, sub,
                    { static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF),
                      static_cast<uint8_t>((value >> 16) & 0xFF),
                      static_cast<uint8_t>((value >> 24) & 0xFF) });
}

SdoResult<void> SdoClient::store_parameters(uint8_t sub)
{
    // "save", little-endian, which on the wire is the ASCII in order:
    // 23 10 10 01 73 61 76 65. The Grayhill manual gives this exact frame.
    log(fmt::format("storing parameters: 0x1010:{:02X} <- \"save\"", sub));
    return download(0x1010, sub, { 's', 'a', 'v', 'e' });
}

SdoResult<void> SdoClient::restore_parameters(uint8_t sub)
{
    return download(0x1011, sub, { 'l', 'o', 'a', 'd' });
}

} // namespace canopen
