// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/control_session_message/*.py
// and         LIVI src/main/services/projection/driver/bt/cp_handler.py
#include "iap2/messages.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>

namespace iap2
{

namespace
{

void put_be16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

uint16_t get_be16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

// ---------------------------------------------------------------------------
// IdentificationInformation parameter identifiers. LIVI derives these from the
// declaration order of the dataclass fields (index 0..17) plus the explicitly
// annotated ones (20, 21, 22, 24, 30).
// ---------------------------------------------------------------------------
constexpr uint16_t kIdName = 0;
constexpr uint16_t kIdModelIdentifier = 1;
constexpr uint16_t kIdManufacturer = 2;
constexpr uint16_t kIdSerialNumber = 3;
constexpr uint16_t kIdFirmwareVersion = 4;
constexpr uint16_t kIdHardwareVersion = 5;
constexpr uint16_t kIdMessagesSent = 6;
constexpr uint16_t kIdMessagesReceived = 7;
constexpr uint16_t kIdPowerProvidingCapability = 8;
constexpr uint16_t kIdMaximumCurrentDrawn = 9;
constexpr uint16_t kIdSupportedEaProtocol = 10;
constexpr uint16_t kIdAppMatchTeamId = 11;
constexpr uint16_t kIdCurrentLanguage = 12;
constexpr uint16_t kIdSupportedLanguage = 13;
constexpr uint16_t kIdSerialTransportComponent = 14;
constexpr uint16_t kIdUsbDeviceTransportComponent = 15;
constexpr uint16_t kIdUsbHostTransportComponent = 16;
constexpr uint16_t kIdBluetoothTransportComponent = 17;
constexpr uint16_t kIdVehicleInformationComponent = 20;
constexpr uint16_t kIdVehicleStatusComponent = 21;
constexpr uint16_t kIdLocationInformationComponent = 22;
constexpr uint16_t kIdWirelessCarPlayTransportComponent = 24;
constexpr uint16_t kIdRouteGuidanceDisplayComponent = 30;

struct IdFieldName
{
    uint16_t id;
    const char* name;
};

constexpr std::array<IdFieldName, 23> kIdFieldNames = {{
    {kIdName, "name"},
    {kIdModelIdentifier, "model_identifier"},
    {kIdManufacturer, "manufacturer"},
    {kIdSerialNumber, "serial_number"},
    {kIdFirmwareVersion, "firmware_version"},
    {kIdHardwareVersion, "hardware_version"},
    {kIdMessagesSent, "messages_sent_by_accessory"},
    {kIdMessagesReceived, "messages_received_from_accessory"},
    {kIdPowerProvidingCapability, "power_providing_capability"},
    {kIdMaximumCurrentDrawn, "maximum_current_drawn_from_device"},
    {kIdSupportedEaProtocol, "supported_external_accessory_protocol"},
    {kIdAppMatchTeamId, "app_match_team_id"},
    {kIdCurrentLanguage, "current_language"},
    {kIdSupportedLanguage, "supported_language"},
    {kIdSerialTransportComponent, "serial_transport_component"},
    {kIdUsbDeviceTransportComponent, "usb_device_transport_component"},
    {kIdUsbHostTransportComponent, "usb_host_transport_component"},
    {kIdBluetoothTransportComponent, "bluetooth_transport_component"},
    {kIdVehicleInformationComponent, "vehicle_information_component"},
    {kIdVehicleStatusComponent, "vehicle_status_component"},
    {kIdLocationInformationComponent, "location_information_component"},
    {kIdWirelessCarPlayTransportComponent, "wireless_car_play_transport_component"},
    {kIdRouteGuidanceDisplayComponent, "route_guidance_display_component"},
}};

// Transport / vehicle / location component sub-parameters.
constexpr uint16_t kComponentId = 0;
constexpr uint16_t kComponentName = 1;
constexpr uint16_t kComponentSupportsIap2 = 2;
constexpr uint16_t kUsbHostCarPlayInterfaceNumber = 3;
constexpr uint16_t kUsbHostSupportsCarPlay = 4;
constexpr uint16_t kVehicleEngineType = 2;
constexpr uint16_t kVehicleStatusRange = 3;
constexpr uint16_t kVehicleStatusOutsideTemperature = 4;
constexpr uint16_t kLocationGpsFixData = 17;
constexpr uint16_t kLocationRecommendedMinimum = 18;

// ExternalAccessoryProtocol sub-parameters.
constexpr uint16_t kEaProtocolId = 0;
constexpr uint16_t kEaProtocolName = 1;
constexpr uint16_t kEaProtocolMatchAction = 2;

// CarPlayAvailability / CarPlayStartSession sub-parameters.
constexpr uint16_t kCarPlayWiredAttributes = 0;
constexpr uint16_t kCarPlayWirelessAttributes = 1;
constexpr uint16_t kCarPlayAvailable = 0;
constexpr uint16_t kCarPlayTransportIdentifier = 1;
constexpr uint16_t kCarPlayIpAddress = 0;
constexpr uint16_t kCarPlayStartPort = 2;
constexpr uint16_t kCarPlayStartDeviceIdentifier = 3;
constexpr uint16_t kCarPlayStartPublicKey = 4;
constexpr uint16_t kCarPlayStartSourceVersion = 5;

// NowPlaying sub-parameters.
constexpr uint16_t kNpMediaItemAttributes = 0;
constexpr uint16_t kNpPlaybackAttributes = 1;
constexpr uint16_t kMiPersistentId = 0;
constexpr uint16_t kMiTitle = 1;
constexpr uint16_t kMiDurationMs = 4;
constexpr uint16_t kMiAlbum = 6;
constexpr uint16_t kMiArtist = 12;
constexpr uint16_t kMiAlbumArtist = 14;
constexpr uint16_t kMiGenre = 16;
constexpr uint16_t kMiArtwork = 26;
constexpr uint16_t kPbStatus = 0;
constexpr uint16_t kPbElapsedMs = 1;
constexpr uint16_t kPbAppName = 7;
constexpr uint16_t kPbAppBundleId = 16;

// RouteGuidance sub-parameters.
constexpr uint16_t kRgDisplayComponentId = 0;
constexpr uint16_t kRgState = 1;
constexpr uint16_t kRgManeuverState = 2;
constexpr uint16_t kRgCurrentRoadName = 3;
constexpr uint16_t kRgDestinationName = 4;
constexpr uint16_t kRgEta = 5;
constexpr uint16_t kRgTimeRemaining = 6;
constexpr uint16_t kRgDistanceRemaining = 7;
constexpr uint16_t kRgDistanceToManeuver = 10;
constexpr uint16_t kRgCurrentManeuverList = 13;
constexpr uint16_t kRmIndex = 1;
constexpr uint16_t kRmManeuverType = 3;
constexpr uint16_t kRmAfterManeuverRoadName = 4;
constexpr uint16_t kRmDrivingSide = 8;
constexpr uint16_t kRmJunctionType = 9;
constexpr uint16_t kRmExitAngle = 11;

// Call state sub-parameters.
constexpr uint16_t kCsRemoteId = 0;
constexpr uint16_t kCsDisplayName = 1;
constexpr uint16_t kCsStatus = 2;
constexpr uint16_t kCsDirection = 3;
constexpr uint16_t kCsCallUuid = 4;
constexpr uint16_t kCsDisconnectReason = 11;

// Communications sub-parameters.
constexpr uint16_t kCommSignalStrength = 0;
constexpr uint16_t kCommRegistrationStatus = 1;
constexpr uint16_t kCommAirplaneMode = 2;
constexpr uint16_t kCommCarrierName = 4;
constexpr uint16_t kCommCellularSupported = 5;

// Power sub-parameters.
constexpr uint16_t kPwMaxCurrentDrawnFromAccessory = 0;
constexpr uint16_t kPwBatteryWillCharge = 1;
constexpr uint16_t kPwAccessoryPowerMode = 2;
constexpr uint16_t kPwExternalChargerConnected = 4;
constexpr uint16_t kPwBatteryChargingState = 5;
constexpr uint16_t kPwBatteryChargeLevel = 6;
constexpr uint16_t kPsAvailableCurrentForDevice = 0;
constexpr uint16_t kPsBatteryShouldCharge = 1;

// Wi-Fi configuration sub-parameters.
constexpr uint16_t kWifiSsid = 1;
constexpr uint16_t kWifiPassphrase = 2;
constexpr uint16_t kWifiSecurityType = 3;
constexpr uint16_t kWifiChannel = 4;

// Location subscription sub-parameters.
constexpr uint16_t kLocNmeaSentence = 0;

// Vehicle status sub-parameters (top level of VehicleStatusUpdate).
constexpr uint16_t kVsRange = 3;
constexpr uint16_t kVsOutsideTemperature = 4;
constexpr uint16_t kVsRangeWarning = 6;

// DeviceTransportIdentifierNotification sub-parameters.
constexpr uint16_t kDtiBluetooth = 0;
constexpr uint16_t kDtiUsb = 1;

// Authentication sub-parameters (single unannotated field -> id 0).
constexpr uint16_t kAuthPayload = 0;

std::vector<uint8_t> messageIdBlob(const std::vector<uint16_t>& ids)
{
    std::vector<uint8_t> blob;
    blob.reserve(ids.size() * 2);
    for (const uint16_t id : ids)
    {
        put_be16(blob, id);
    }
    return blob;
}

csm::ParamList usbHostTransportComponent(const IdentificationConfig& config)
{
    csm::ParamList component;
    csm::addU16(component, kComponentId, config.usb_host_component_id);
    csm::addString(component, kComponentName, config.usb_host_component_name);
    csm::addNone(component, kComponentSupportsIap2);
    csm::addU8(component, kUsbHostCarPlayInterfaceNumber, config.car_play_interface_number);
    if (config.supports_car_play)
    {
        csm::addNone(component, kUsbHostSupportsCarPlay);
    }
    return component;
}

}  // namespace

const char* messageIdName(uint16_t msg_id)
{
    switch (msg_id)
    {
        case kMsgStartIdentification: return "StartIdentification";
        case kMsgIdentificationInformation: return "IdentificationInformation";
        case kMsgIdentificationAccepted: return "IdentificationAccepted";
        case kMsgIdentificationRejected: return "IdentificationRejected";
        case kMsgRequestAuthenticationCertificate: return "RequestAuthenticationCertificate";
        case kMsgAuthenticationCertificate: return "AuthenticationCertificate";
        case kMsgRequestAuthenticationChallengeResponse: return "RequestAuthenticationChallengeResponse";
        case kMsgAuthenticationResponse: return "AuthenticationResponse";
        case kMsgAuthenticationFailed: return "AuthenticationFailed";
        case kMsgAuthenticationSucceeded: return "AuthenticationSucceeded";
        case kMsgCarPlayAvailability: return "CarPlayAvailability";
        case kMsgCarPlayStartSession: return "CarPlayStartSession";
        case kMsgWirelessCarPlayUpdate: return "WirelessCarPlayUpdate";
        case kMsgDeviceTransportIdentifierNotification: return "DeviceTransportIdentifierNotification";
        case kMsgStartCallStateUpdates: return "StartCallStateUpdates";
        case kMsgCallStateUpdate: return "CallStateUpdate";
        case kMsgStopCallStateUpdates: return "StopCallStateUpdates";
        case kMsgStartCommunicationsUpdates: return "StartCommunicationsUpdates";
        case kMsgCommunicationsUpdate: return "CommunicationsUpdate";
        case kMsgStopCommunicationsUpdates: return "StopCommunicationsUpdates";
        case kMsgStartNowPlayingUpdates: return "StartNowPlayingUpdates";
        case kMsgNowPlayingUpdate: return "NowPlayingUpdate";
        case kMsgStopNowPlayingUpdates: return "StopNowPlayingUpdates";
        case kMsgStartRouteGuidanceUpdates: return "StartRouteGuidanceUpdates";
        case kMsgRouteGuidanceUpdate: return "RouteGuidanceUpdate";
        case kMsgRouteGuidanceManeuverUpdate: return "RouteGuidanceManeuverUpdate";
        case kMsgStopRouteGuidanceUpdates: return "StopRouteGuidanceUpdates";
        case kMsgStartPowerUpdates: return "StartPowerUpdates";
        case kMsgPowerUpdate: return "PowerUpdate";
        case kMsgStopPowerUpdates: return "StopPowerUpdates";
        case kMsgPowerSourceUpdate: return "PowerSourceUpdate";
        case kMsgStartVehicleStatusUpdates: return "StartVehicleStatusUpdates";
        case kMsgVehicleStatusUpdate: return "VehicleStatusUpdate";
        case kMsgStopVehicleStatusUpdates: return "StopVehicleStatusUpdates";
        case kMsgStartLocationInformation: return "StartLocationInformation";
        case kMsgLocationInformation: return "LocationInformation";
        case kMsgStopLocationInformation: return "StopLocationInformation";
        case kMsgRequestWiFiInformation: return "RequestWiFiInformation";
        case kMsgWiFiInformation: return "WiFiInformation";
        case kMsgRequestAccessoryWiFiConfigurationInformation:
            return "RequestAccessoryWiFiConfigurationInformation";
        case kMsgAccessoryWiFiConfigurationInformation: return "AccessoryWiFiConfigurationInformation";
        case kMsgStartExternalAccessoryProtocolSession: return "StartExternalAccessoryProtocolSession";
        case kMsgStopExternalAccessoryProtocolSession: return "StopExternalAccessoryProtocolSession";
        case kMsgStatusExternalAccessoryProtocolSession: return "StatusExternalAccessoryProtocolSession";
        default: return "unknown";
    }
}

namespace csm
{

namespace
{

template <typename T>
void addBigEndian(ParamList& params, uint16_t id, T value)
{
    Param param;
    param.id = id;
    param.data.resize(sizeof(T));
    for (size_t i = 0; i < sizeof(T); ++i)
    {
        param.data[sizeof(T) - 1 - i] = static_cast<uint8_t>(value >> (8 * i));
    }
    params.push_back(std::move(param));
}

template <typename T>
std::optional<T> getBigEndian(const ParamList& params, uint16_t id)
{
    const Param* param = find(params, id);
    if (param == nullptr)
    {
        return std::nullopt;
    }
    if (param->data.size() != sizeof(T))
    {
        SPDLOG_WARN("[iap2] parameter {} has {} bytes, expected {}", id, param->data.size(), sizeof(T));
        return std::nullopt;
    }

    T value = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
    {
        value = static_cast<T>((static_cast<uint64_t>(value) << 8) | param->data[i]);
    }
    return value;
}

}  // namespace

void addNone(ParamList& params, uint16_t id) { params.push_back(Param{id, {}}); }

void addBool(ParamList& params, uint16_t id, bool value)
{
    params.push_back(Param{id, {static_cast<uint8_t>(value ? 1 : 0)}});
}

void addU8(ParamList& params, uint16_t id, uint8_t value) { params.push_back(Param{id, {value}}); }

void addI8(ParamList& params, uint16_t id, int8_t value)
{
    params.push_back(Param{id, {static_cast<uint8_t>(value)}});
}

void addU16(ParamList& params, uint16_t id, uint16_t value) { addBigEndian<uint16_t>(params, id, value); }

void addI16(ParamList& params, uint16_t id, int16_t value)
{
    addBigEndian<uint16_t>(params, id, static_cast<uint16_t>(value));
}

void addU32(ParamList& params, uint16_t id, uint32_t value) { addBigEndian<uint32_t>(params, id, value); }

void addU64(ParamList& params, uint16_t id, uint64_t value) { addBigEndian<uint64_t>(params, id, value); }

void addEnum(ParamList& params, uint16_t id, uint8_t value) { addU8(params, id, value); }

void addString(ParamList& params, uint16_t id, std::string_view value)
{
    Param param;
    param.id = id;
    param.data.assign(value.begin(), value.end());
    param.data.push_back(0);
    params.push_back(std::move(param));
}

void addBytes(ParamList& params, uint16_t id, const std::vector<uint8_t>& value)
{
    params.push_back(Param{id, value});
}

void addGroup(ParamList& params, uint16_t id, const ParamList& group)
{
    params.push_back(Param{id, encodeParams(group)});
}

std::vector<uint8_t> encodeParams(const ParamList& params)
{
    std::vector<uint8_t> out;
    for (const Param& param : params)
    {
        const size_t total = param.data.size() + kCsmParamHeaderSize;
        if (total > 0xFFFF)
        {
            SPDLOG_ERROR("[iap2] parameter {} is {} bytes, too large to encode", param.id, param.data.size());
            continue;
        }
        put_be16(out, static_cast<uint16_t>(total));
        put_be16(out, param.id);
        out.insert(out.end(), param.data.begin(), param.data.end());
    }
    return out;
}

std::vector<uint8_t> encodeMessage(uint16_t msg_id, const ParamList& params)
{
    const std::vector<uint8_t> body = encodeParams(params);
    const size_t total = body.size() + kCsmHeaderSize;
    if (total > 0xFFFF)
    {
        SPDLOG_ERROR("[iap2] message 0x{:04X} ({}) is {} bytes, too large to encode", msg_id,
                     messageIdName(msg_id), total);
        return {};
    }

    std::vector<uint8_t> out;
    out.reserve(total);
    put_be16(out, kCsmStart);
    put_be16(out, static_cast<uint16_t>(total));
    put_be16(out, msg_id);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::optional<ParamList> parseParams(const uint8_t* data, size_t len)
{
    ParamList params;
    size_t offset = 0;
    while (offset < len)
    {
        if (len - offset < kCsmParamHeaderSize)
        {
            SPDLOG_WARN("[iap2] truncated parameter header: {} trailing bytes", len - offset);
            return std::nullopt;
        }

        const uint16_t total = get_be16(data + offset);
        const uint16_t id = get_be16(data + offset + 2);
        if (total < kCsmParamHeaderSize || total > len - offset)
        {
            SPDLOG_WARN("[iap2] parameter {} declares length {} but only {} bytes remain", id, total,
                        len - offset);
            return std::nullopt;
        }

        Param param;
        param.id = id;
        param.data.assign(data + offset + kCsmParamHeaderSize, data + offset + total);
        params.push_back(std::move(param));
        offset += total;
    }
    return params;
}

std::optional<Message> parseMessage(const uint8_t* data, size_t len)
{
    if (len < kCsmHeaderSize)
    {
        SPDLOG_WARN("[iap2] control message is only {} bytes, need at least {}", len, kCsmHeaderSize);
        return std::nullopt;
    }

    const uint16_t start = get_be16(data);
    const uint16_t total = get_be16(data + 2);
    const uint16_t msg_id = get_be16(data + 4);
    if (start != kCsmStart)
    {
        SPDLOG_WARN("[iap2] control message has bad start marker 0x{:04X} (expected 0x{:04X})", start,
                    kCsmStart);
        return std::nullopt;
    }
    if (total < kCsmHeaderSize || total > len)
    {
        SPDLOG_WARN("[iap2] control message 0x{:04X} declares length {} but {} bytes are available", msg_id,
                    total, len);
        return std::nullopt;
    }

    auto params = parseParams(data + kCsmHeaderSize, total - kCsmHeaderSize);
    if (!params)
    {
        SPDLOG_WARN("[iap2] control message 0x{:04X} ({}) has malformed parameters", msg_id,
                    messageIdName(msg_id));
        return std::nullopt;
    }

    Message message;
    message.id = msg_id;
    message.params = std::move(*params);
    return message;
}

std::optional<Message> parseMessage(const std::vector<uint8_t>& frame)
{
    return parseMessage(frame.data(), frame.size());
}

std::optional<size_t> peekLength(const uint8_t* data, size_t len)
{
    if (len < kCsmHeaderSize)
    {
        return std::nullopt;
    }
    if (get_be16(data) != kCsmStart)
    {
        return size_t{0};
    }
    const uint16_t total = get_be16(data + 2);
    if (total < kCsmHeaderSize)
    {
        return size_t{0};
    }
    return static_cast<size_t>(total);
}

const Param* find(const ParamList& params, uint16_t id)
{
    for (const Param& param : params)
    {
        if (param.id == id)
        {
            return &param;
        }
    }
    return nullptr;
}

bool has(const ParamList& params, uint16_t id) { return find(params, id) != nullptr; }

std::optional<bool> getBool(const ParamList& params, uint16_t id)
{
    const Param* param = find(params, id);
    if (param == nullptr)
    {
        return std::nullopt;
    }
    if (param->data.empty())
    {
        // LIVI decodes a zero-length boolean as None. Kept faithful, but logged
        // because the iAP2 spec does allow presence-as-value here.
        SPDLOG_DEBUG("[iap2] boolean parameter {} is zero length, treating as absent", id);
        return std::nullopt;
    }
    return param->data[0] != 0;
}

std::optional<uint8_t> getU8(const ParamList& params, uint16_t id)
{
    return getBigEndian<uint8_t>(params, id);
}

std::optional<int8_t> getI8(const ParamList& params, uint16_t id)
{
    const auto value = getBigEndian<uint8_t>(params, id);
    if (!value)
    {
        return std::nullopt;
    }
    return static_cast<int8_t>(*value);
}

std::optional<uint16_t> getU16(const ParamList& params, uint16_t id)
{
    return getBigEndian<uint16_t>(params, id);
}

std::optional<int16_t> getI16(const ParamList& params, uint16_t id)
{
    const auto value = getBigEndian<uint16_t>(params, id);
    if (!value)
    {
        return std::nullopt;
    }
    return static_cast<int16_t>(*value);
}

std::optional<uint32_t> getU32(const ParamList& params, uint16_t id)
{
    return getBigEndian<uint32_t>(params, id);
}

std::optional<uint64_t> getU64(const ParamList& params, uint16_t id)
{
    return getBigEndian<uint64_t>(params, id);
}

std::optional<std::string> getString(const ParamList& params, uint16_t id)
{
    const Param* param = find(params, id);
    if (param == nullptr)
    {
        return std::nullopt;
    }

    size_t len = param->data.size();
    if (len > 0 && param->data[len - 1] == 0)
    {
        --len;
    }
    return std::string(reinterpret_cast<const char*>(param->data.data()), len);
}

std::optional<std::vector<uint8_t>> getBytes(const ParamList& params, uint16_t id)
{
    const Param* param = find(params, id);
    if (param == nullptr)
    {
        return std::nullopt;
    }
    return param->data;
}

std::optional<ParamList> getGroup(const ParamList& params, uint16_t id)
{
    const Param* param = find(params, id);
    if (param == nullptr)
    {
        return std::nullopt;
    }
    auto group = parseParams(param->data.data(), param->data.size());
    if (!group)
    {
        SPDLOG_WARN("[iap2] nested parameter group {} is malformed ({} bytes)", id, param->data.size());
    }
    return group;
}

}  // namespace csm

// ---------------------------------------------------------------------------
// Identification.
// ---------------------------------------------------------------------------

std::vector<uint16_t> identificationMessagesSent(const IdentificationConfig& config)
{
    std::vector<uint16_t> ids = {
        kMsgVehicleStatusUpdate,
        kMsgAccessoryWiFiConfigurationInformation,
        kMsgStartNowPlayingUpdates,
        kMsgStopNowPlayingUpdates,
        kMsgStartRouteGuidanceUpdates,
        kMsgStopRouteGuidanceUpdates,
        kMsgLocationInformation,
        kMsgStartPowerUpdates,
        kMsgStopPowerUpdates,
        kMsgStartCommunicationsUpdates,
        kMsgStopCommunicationsUpdates,
        kMsgStartCallStateUpdates,
        kMsgStopCallStateUpdates,
        // Wired accessories provide power, so they also send PowerSourceUpdate.
        kMsgPowerSourceUpdate,
    };
    if (config.carplay_wired_start_session)
    {
        ids.push_back(kMsgCarPlayStartSession);
    }
    return ids;
}

std::vector<uint16_t> identificationMessagesReceived(const IdentificationConfig& config)
{
    std::vector<uint16_t> ids = {
        kMsgStartExternalAccessoryProtocolSession,
        kMsgStopExternalAccessoryProtocolSession,
        kMsgStartVehicleStatusUpdates,
        kMsgStopVehicleStatusUpdates,
        kMsgWirelessCarPlayUpdate,
        kMsgDeviceTransportIdentifierNotification,
        kMsgRequestAccessoryWiFiConfigurationInformation,
        kMsgNowPlayingUpdate,
        kMsgRouteGuidanceUpdate,
        kMsgRouteGuidanceManeuverUpdate,
        kMsgStartLocationInformation,
        kMsgStopLocationInformation,
        kMsgPowerUpdate,
        kMsgCommunicationsUpdate,
        kMsgCallStateUpdate,
    };
    if (config.carplay_wired_start_session)
    {
        ids.push_back(kMsgCarPlayAvailability);
    }
    return ids;
}

std::vector<uint8_t> encodeIdentificationInformation(const IdentificationConfig& config)
{
    csm::ParamList params;
    csm::addString(params, kIdName, config.name);
    csm::addString(params, kIdModelIdentifier, config.model_identifier);
    csm::addString(params, kIdManufacturer, config.manufacturer);
    csm::addString(params, kIdSerialNumber, config.serial_number);
    csm::addString(params, kIdFirmwareVersion, config.firmware_version);
    csm::addString(params, kIdHardwareVersion, config.hardware_version);
    csm::addBytes(params, kIdMessagesSent, messageIdBlob(identificationMessagesSent(config)));
    csm::addBytes(params, kIdMessagesReceived, messageIdBlob(identificationMessagesReceived(config)));
    csm::addEnum(params, kIdPowerProvidingCapability,
                 static_cast<uint8_t>(config.power_providing_capability));
    csm::addU16(params, kIdMaximumCurrentDrawn, config.maximum_current_drawn_from_device);

    {
        csm::ParamList protocol;
        csm::addU8(protocol, kEaProtocolId, config.ea_protocol_id);
        csm::addString(protocol, kEaProtocolName, config.ea_protocol_name);
        csm::addEnum(protocol, kEaProtocolMatchAction, static_cast<uint8_t>(config.ea_match_action));
        csm::addGroup(params, kIdSupportedEaProtocol, protocol);
    }

    csm::addString(params, kIdCurrentLanguage, config.current_language);
    for (const std::string& language : config.supported_languages)
    {
        csm::addString(params, kIdSupportedLanguage, language);
    }

    csm::addGroup(params, kIdUsbHostTransportComponent, usbHostTransportComponent(config));

    if (config.include_vehicle_information)
    {
        csm::ParamList component;
        csm::addU16(component, kComponentId, 0);
        csm::addString(component, kComponentName, config.name);
        csm::addEnum(component, kVehicleEngineType, static_cast<uint8_t>(config.engine_type));
        csm::addGroup(params, kIdVehicleInformationComponent, component);
    }

    if (config.include_vehicle_status)
    {
        csm::ParamList component;
        csm::addU16(component, kComponentId, 0);
        csm::addString(component, kComponentName, config.name);
        csm::addNone(component, kVehicleStatusRange);
        csm::addNone(component, kVehicleStatusOutsideTemperature);
        csm::addGroup(params, kIdVehicleStatusComponent, component);
    }

    if (config.include_location_information)
    {
        csm::ParamList component;
        csm::addU16(component, kComponentId, 0);
        csm::addString(component, kComponentName, config.name);
        csm::addNone(component, kLocationGpsFixData);
        csm::addNone(component, kLocationRecommendedMinimum);
        csm::addGroup(params, kIdLocationInformationComponent, component);
    }

    if (config.include_route_guidance_display)
    {
        csm::ParamList component;
        csm::addU16(component, kComponentId, 0);
        csm::addString(component, kComponentName, config.name);
        csm::addGroup(params, kIdRouteGuidanceDisplayComponent, component);
    }

    auto frame = csm::encodeMessage(kMsgIdentificationInformation, params);
    SPDLOG_DEBUG("[iap2] IdentificationInformation encoded: {} bytes, {} params, carplay_iface={} "
                 "power_cap={} start_session={}",
                 frame.size(), params.size(), config.car_play_interface_number,
                 static_cast<int>(config.power_providing_capability), config.carplay_wired_start_session);
    return frame;
}

bool IdentificationRejection::contains(uint16_t param_id) const
{
    return std::find(flagged_params.begin(), flagged_params.end(), param_id) != flagged_params.end();
}

std::optional<IdentificationRejection> decodeIdentificationRejected(const csm::ParamList& params)
{
    IdentificationRejection rejection;
    for (const csm::Param& param : params)
    {
        rejection.flagged_params.push_back(param.id);

        const char* name = "unknown";
        for (const IdFieldName& field : kIdFieldNames)
        {
            if (field.id == param.id)
            {
                name = field.name;
                break;
            }
        }
        rejection.flagged_names.emplace_back(name);
    }

    SPDLOG_WARN("[iap2] IdentificationRejected: {} field(s) flagged", rejection.flagged_names.size());
    for (size_t i = 0; i < rejection.flagged_names.size(); ++i)
    {
        SPDLOG_WARN("[iap2]   rejected param {} ({})", rejection.flagged_params[i],
                    rejection.flagged_names[i]);
    }
    return rejection;
}

bool applyIdentificationRejection(const IdentificationRejection& rejection, IdentificationConfig& config)
{
    bool dropped = false;

    if (rejection.contains(kIdLocationInformationComponent) && config.include_location_information)
    {
        config.include_location_information = false;
        dropped = true;
        SPDLOG_WARN("[iap2] retrying identification without location_information_component");
    }
    if (rejection.contains(kIdVehicleInformationComponent) && config.include_vehicle_information)
    {
        config.include_vehicle_information = false;
        dropped = true;
        SPDLOG_WARN("[iap2] retrying identification without vehicle_information_component");
    }
    if (rejection.contains(kIdVehicleStatusComponent) && config.include_vehicle_status)
    {
        config.include_vehicle_status = false;
        dropped = true;
        SPDLOG_WARN("[iap2] retrying identification without vehicle_status_component");
    }
    if (rejection.contains(kIdRouteGuidanceDisplayComponent) && config.include_route_guidance_display)
    {
        config.include_route_guidance_display = false;
        dropped = true;
        SPDLOG_WARN("[iap2] retrying identification without route_guidance_display_component");
    }

    if (!dropped)
    {
        SPDLOG_ERROR("[iap2] identification rejected and nothing left to drop, giving up");
    }
    return dropped;
}

// ---------------------------------------------------------------------------
// Authentication.
// ---------------------------------------------------------------------------

std::vector<uint8_t> encodeAuthenticationCertificate(const std::vector<uint8_t>& certificate)
{
    csm::ParamList params;
    csm::addBytes(params, kAuthPayload, certificate);
    return csm::encodeMessage(kMsgAuthenticationCertificate, params);
}

std::vector<uint8_t> encodeAuthenticationResponse(const std::vector<uint8_t>& response)
{
    csm::ParamList params;
    csm::addBytes(params, kAuthPayload, response);
    return csm::encodeMessage(kMsgAuthenticationResponse, params);
}

std::optional<std::vector<uint8_t>> decodeAuthenticationChallenge(const csm::ParamList& params)
{
    auto challenge = csm::getBytes(params, kAuthPayload);
    if (!challenge)
    {
        SPDLOG_ERROR("[mfi] RequestAuthenticationChallengeResponse has no challenge parameter");
        return std::nullopt;
    }
    return challenge;
}

MfiAuthenticator::MfiAuthenticator(MfiSigner& signer) : signer_(signer) {}

MfiAuthenticator::Result MfiAuthenticator::handle(const csm::Message& message, std::vector<uint8_t>& reply)
{
    switch (message.id)
    {
        case kMsgRequestAuthenticationCertificate:
        {
            SPDLOG_DEBUG("[mfi] RequestAuthenticationCertificate, reading coprocessor certificate");
            const auto certificate = signer_.certificate();
            if (!certificate || certificate->empty())
            {
                SPDLOG_ERROR("[mfi] coprocessor returned no certificate, authentication cannot proceed");
                return Result::kFailed;
            }
            reply = encodeAuthenticationCertificate(*certificate);
            SPDLOG_DEBUG("[mfi] sending AuthenticationCertificate, {} bytes", certificate->size());
            return Result::kReply;
        }

        case kMsgRequestAuthenticationChallengeResponse:
        {
            const auto challenge = decodeAuthenticationChallenge(message.params);
            if (!challenge)
            {
                return Result::kFailed;
            }

            const int major = signer_.protocolMajor();
            const size_t expected = (major >= 3) ? 32u : 20u;
            if (challenge->size() != expected)
            {
                // Not fatal: sign what the device sent and let it judge. Logged
                // loudly because a mismatch usually means the coprocessor
                // version was misdetected.
                SPDLOG_WARN("[mfi] challenge is {} bytes but protocol major {} expects {}",
                            challenge->size(), major, expected);
            }

            const auto response = signer_.signChallenge(*challenge);
            if (!response || response->empty())
            {
                SPDLOG_ERROR("[mfi] coprocessor failed to sign the {} byte challenge", challenge->size());
                return Result::kFailed;
            }
            reply = encodeAuthenticationResponse(*response);
            SPDLOG_DEBUG("[mfi] sending AuthenticationResponse, {} bytes (challenge {} bytes, major {})",
                         response->size(), challenge->size(), major);
            return Result::kReply;
        }

        case kMsgAuthenticationSucceeded:
            SPDLOG_INFO("[mfi] authentication succeeded");
            return Result::kSucceeded;

        case kMsgAuthenticationFailed:
            SPDLOG_ERROR("[mfi] device reported AuthenticationFailed");
            return Result::kFailed;

        default:
            return Result::kIgnored;
    }
}

// ---------------------------------------------------------------------------
// CarPlay session control.
// ---------------------------------------------------------------------------

std::optional<CarPlayAvailability> decodeCarPlayAvailability(const csm::ParamList& params)
{
    CarPlayAvailability availability;

    if (const auto wired = csm::getGroup(params, kCarPlayWiredAttributes))
    {
        availability.has_wired = true;
        availability.wired_available = csm::getBool(*wired, kCarPlayAvailable);
        availability.usb_transport_identifier = csm::getString(*wired, kCarPlayTransportIdentifier);
    }
    if (const auto wireless = csm::getGroup(params, kCarPlayWirelessAttributes))
    {
        availability.has_wireless = true;
        availability.wireless_available = csm::getBool(*wireless, kCarPlayAvailable);
        availability.bluetooth_transport_identifier = csm::getString(*wireless, kCarPlayTransportIdentifier);
    }

    SPDLOG_DEBUG("[iap2] CarPlayAvailability wired={} available={} wireless={} available={}",
                 availability.has_wired,
                 availability.wired_available ? (*availability.wired_available ? 1 : 0) : -1,
                 availability.has_wireless,
                 availability.wireless_available ? (*availability.wireless_available ? 1 : 0) : -1);
    return availability;
}

std::vector<uint8_t> encodeCarPlayStartSession(const CarPlayStartSession& session)
{
    csm::ParamList params;

    csm::ParamList wired;
    for (const std::string& address : session.ip_addresses)
    {
        csm::addString(wired, kCarPlayIpAddress, address);
    }
    csm::addGroup(params, kCarPlayWiredAttributes, wired);

    csm::addU32(params, kCarPlayStartPort, session.port);
    csm::addString(params, kCarPlayStartDeviceIdentifier, session.device_identifier);
    csm::addString(params, kCarPlayStartPublicKey, session.public_key);
    csm::addString(params, kCarPlayStartSourceVersion, session.source_version);

    SPDLOG_DEBUG("[iap2] CarPlayStartSession addr={} port={} bt={} src={}",
                 session.ip_addresses.empty() ? "<none>" : session.ip_addresses.front(), session.port,
                 session.device_identifier, session.source_version);
    return csm::encodeMessage(kMsgCarPlayStartSession, params);
}

std::optional<DeviceTransportIdentifiers> decodeDeviceTransportIdentifierNotification(
    const csm::ParamList& params)
{
    DeviceTransportIdentifiers identifiers;
    identifiers.bluetooth_transport_id = csm::getString(params, kDtiBluetooth);
    identifiers.usb_transport_id = csm::getString(params, kDtiUsb);
    SPDLOG_DEBUG("[iap2] DeviceTransportIdentifierNotification bt='{}' usb='{}'",
                 identifiers.bluetooth_transport_id.value_or(""),
                 identifiers.usb_transport_id.value_or(""));
    return identifiers;
}

std::optional<WirelessCarPlayStatus> decodeWirelessCarPlayUpdate(const csm::ParamList& params)
{
    const auto status = csm::getU8(params, 0);
    if (!status)
    {
        SPDLOG_WARN("[iap2] WirelessCarPlayUpdate has no status parameter");
        return std::nullopt;
    }
    return static_cast<WirelessCarPlayStatus>(*status);
}

// ---------------------------------------------------------------------------
// Now playing.
// ---------------------------------------------------------------------------

std::optional<NowPlaying> decodeNowPlayingUpdate(const csm::ParamList& params)
{
    NowPlaying now_playing;

    if (const auto media = csm::getGroup(params, kNpMediaItemAttributes))
    {
        now_playing.has_media_item = true;
        now_playing.persistent_id = csm::getU64(*media, kMiPersistentId);
        now_playing.title = csm::getString(*media, kMiTitle);
        now_playing.duration_ms = csm::getU32(*media, kMiDurationMs);
        now_playing.album = csm::getString(*media, kMiAlbum);
        now_playing.artist = csm::getString(*media, kMiArtist);
        now_playing.album_artist = csm::getString(*media, kMiAlbumArtist);
        now_playing.genre = csm::getString(*media, kMiGenre);
        now_playing.artwork_ftid = csm::getU8(*media, kMiArtwork);
    }

    if (const auto playback = csm::getGroup(params, kNpPlaybackAttributes))
    {
        now_playing.has_playback = true;
        if (const auto status = csm::getU8(*playback, kPbStatus))
        {
            now_playing.status = static_cast<PlaybackStatus>(*status);
        }
        now_playing.elapsed_ms = csm::getU32(*playback, kPbElapsedMs);
        now_playing.app_name = csm::getString(*playback, kPbAppName);
        now_playing.app_bundle_id = csm::getString(*playback, kPbAppBundleId);
    }

    if (!now_playing.has_media_item && !now_playing.has_playback)
    {
        SPDLOG_DEBUG("[iap2] NowPlayingUpdate carried neither media item nor playback attributes");
    }
    return now_playing;
}

std::vector<uint8_t> encodeStartNowPlayingUpdates()
{
    csm::ParamList media;
    csm::addNone(media, kMiTitle);
    csm::addNone(media, kMiArtist);
    csm::addNone(media, kMiAlbum);
    csm::addNone(media, kMiDurationMs);
    csm::addNone(media, kMiArtwork);

    csm::ParamList playback;
    csm::addNone(playback, kPbStatus);
    csm::addNone(playback, kPbElapsedMs);
    csm::addNone(playback, kPbAppName);

    csm::ParamList params;
    csm::addGroup(params, kNpMediaItemAttributes, media);
    csm::addGroup(params, kNpPlaybackAttributes, playback);
    return csm::encodeMessage(kMsgStartNowPlayingUpdates, params);
}

std::vector<uint8_t> encodeStopNowPlayingUpdates()
{
    return csm::encodeMessage(kMsgStopNowPlayingUpdates, {});
}

// ---------------------------------------------------------------------------
// Route guidance.
// ---------------------------------------------------------------------------

std::optional<RouteGuidance> decodeRouteGuidanceUpdate(const csm::ParamList& params)
{
    RouteGuidance guidance;
    guidance.display_component_id = csm::getU16(params, kRgDisplayComponentId);
    guidance.state = csm::getU8(params, kRgState);
    guidance.maneuver_state = csm::getU8(params, kRgManeuverState);
    guidance.current_road_name = csm::getString(params, kRgCurrentRoadName);
    guidance.destination_name = csm::getString(params, kRgDestinationName);
    guidance.eta = csm::getU64(params, kRgEta);
    guidance.time_remaining = csm::getU64(params, kRgTimeRemaining);
    guidance.distance_remaining = csm::getU32(params, kRgDistanceRemaining);
    guidance.distance_to_maneuver = csm::getU32(params, kRgDistanceToManeuver);
    guidance.current_maneuver_list = csm::getBytes(params, kRgCurrentManeuverList);
    return guidance;
}

std::optional<RouteManeuver> decodeRouteGuidanceManeuverUpdate(const csm::ParamList& params)
{
    RouteManeuver maneuver;
    maneuver.display_component_id = csm::getU16(params, kRgDisplayComponentId);
    maneuver.index = csm::getU16(params, kRmIndex);
    maneuver.maneuver_type = csm::getU8(params, kRmManeuverType);
    maneuver.after_maneuver_road_name = csm::getString(params, kRmAfterManeuverRoadName);
    maneuver.driving_side = csm::getU8(params, kRmDrivingSide);
    maneuver.junction_type = csm::getU8(params, kRmJunctionType);
    maneuver.exit_angle = csm::getI16(params, kRmExitAngle);
    return maneuver;
}

std::vector<uint8_t> encodeStartRouteGuidanceUpdates()
{
    // LIVI sends StartRouteGuidanceUpdates() with no display component id, so
    // the phone reports every display component.
    return csm::encodeMessage(kMsgStartRouteGuidanceUpdates, {});
}

std::vector<uint8_t> encodeStopRouteGuidanceUpdates()
{
    return csm::encodeMessage(kMsgStopRouteGuidanceUpdates, {});
}

void NavGuidance::apply(const RouteGuidance& update)
{
    if (update.state)
    {
        status = update.state;
    }
    if (update.maneuver_state)
    {
        order_type = update.maneuver_state;
    }
    if (update.current_road_name)
    {
        road_name = update.current_road_name;
    }
    if (update.destination_name)
    {
        destination_name = update.destination_name;
    }
    if (update.eta)
    {
        eta_epoch = update.eta;
    }
    if (update.time_remaining)
    {
        time_to_destination = update.time_remaining;
    }
    if (update.distance_remaining)
    {
        distance_to_destination = update.distance_remaining;
    }
    if (update.distance_to_maneuver)
    {
        remain_distance = update.distance_to_maneuver;
    }
    if (update.current_maneuver_list && update.current_maneuver_list->size() >= 2)
    {
        const auto& list = *update.current_maneuver_list;
        current_index = static_cast<uint16_t>((static_cast<uint16_t>(list[0]) << 8) | list[1]);
    }

    refreshCurrent();
    SPDLOG_DEBUG("[iap2] nav: road='{}' dest='{}' remain={}m maneuver_index={}", road_name.value_or(""),
                 destination_name.value_or(""), remain_distance.value_or(0),
                 current_index ? static_cast<int>(*current_index) : -1);
}

void NavGuidance::apply(const RouteManeuver& update)
{
    if (!update.index)
    {
        SPDLOG_WARN("[iap2] RouteGuidanceManeuverUpdate without an index, ignoring");
        return;
    }

    Maneuver& maneuver = maneuvers_[*update.index];
    if (update.maneuver_type)
    {
        maneuver.maneuver_type = update.maneuver_type;
    }
    if (update.driving_side)
    {
        maneuver.turn_side = update.driving_side;
    }
    if (update.junction_type)
    {
        maneuver.junction_type = update.junction_type;
    }
    if (update.exit_angle)
    {
        maneuver.turn_angle = update.exit_angle;
    }
    if (update.after_maneuver_road_name)
    {
        maneuver.after_road_name = update.after_maneuver_road_name;
    }

    refreshCurrent();
}

void NavGuidance::refreshCurrent()
{
    if (!current_index)
    {
        return;
    }
    const auto it = maneuvers_.find(*current_index);
    if (it == maneuvers_.end())
    {
        return;
    }

    maneuver_type = it->second.maneuver_type;
    turn_side = it->second.turn_side;
    junction_type = it->second.junction_type;
    turn_angle = it->second.turn_angle;
    after_road_name = it->second.after_road_name;
}

// ---------------------------------------------------------------------------
// Call state.
// ---------------------------------------------------------------------------

std::optional<CallState> decodeCallStateUpdate(const csm::ParamList& params)
{
    CallState state;
    state.remote_id = csm::getString(params, kCsRemoteId);
    state.display_name = csm::getString(params, kCsDisplayName);
    state.status = csm::getU8(params, kCsStatus);
    state.direction = csm::getU8(params, kCsDirection);
    state.call_uuid = csm::getString(params, kCsCallUuid);
    state.disconnect_reason = csm::getU8(params, kCsDisconnectReason);
    return state;
}

std::vector<uint8_t> encodeStartCallStateUpdates()
{
    csm::ParamList params;
    csm::addNone(params, kCsRemoteId);
    csm::addNone(params, kCsDisplayName);
    csm::addNone(params, kCsStatus);
    csm::addNone(params, kCsDirection);
    csm::addNone(params, kCsCallUuid);
    csm::addNone(params, kCsDisconnectReason);
    return csm::encodeMessage(kMsgStartCallStateUpdates, params);
}

std::vector<uint8_t> encodeStopCallStateUpdates()
{
    return csm::encodeMessage(kMsgStopCallStateUpdates, {});
}

const char* CallTracker::phaseName(Phase phase)
{
    switch (phase)
    {
        case Phase::kEnded: return "ended";
        case Phase::kRinging: return "ringing";
        case Phase::kActive: return "active";
    }
    return "?";
}

bool CallTracker::apply(const CallState& update)
{
    if (!update.status)
    {
        SPDLOG_DEBUG("[iap2] CallStateUpdate without a status, ignoring");
        return false;
    }

    const std::string uuid = update.call_uuid.value_or("_");
    if (*update.status == 0)
    {
        calls_.erase(uuid);
    }
    else
    {
        Call& call = calls_[uuid];
        call.status = *update.status;
        call.number = update.remote_id.value_or(call.number);
        call.name = update.display_name.value_or(call.name);
    }

    const auto is_active = [](uint8_t status)
    { return status == 1 || (status >= 3 && status <= 6); };

    bool active = false;
    bool ringing = false;
    for (const auto& [uuid_key, call] : calls_)
    {
        (void)uuid_key;
        active = active || is_active(call.status);
        ringing = ringing || call.status == 2;
    }

    const Phase phase = active ? Phase::kActive : (ringing ? Phase::kRinging : Phase::kEnded);
    if (phase == phase_)
    {
        return false;
    }
    phase_ = phase;

    number_.clear();
    name_.clear();
    if (phase_ != Phase::kEnded)
    {
        for (const auto& [uuid_key, call] : calls_)
        {
            (void)uuid_key;
            const bool match = (phase_ == Phase::kRinging) ? call.status == 2 : is_active(call.status);
            if (match)
            {
                number_ = call.number;
                name_ = call.name;
                break;
            }
        }
    }

    SPDLOG_DEBUG("[iap2] call phase={} name='{}' number='{}' ({} tracked)", phaseName(phase_), name_,
                 number_, calls_.size());
    return true;
}

// ---------------------------------------------------------------------------
// Power.
// ---------------------------------------------------------------------------

std::optional<PowerState> decodePowerUpdate(const csm::ParamList& params)
{
    PowerState state;
    state.maximum_current_drawn_from_accessory = csm::getU16(params, kPwMaxCurrentDrawnFromAccessory);
    state.device_battery_will_charge_if_power_is_present = csm::getBool(params, kPwBatteryWillCharge);
    state.accessory_power_mode = csm::getU8(params, kPwAccessoryPowerMode);
    state.is_external_charger_connected = csm::getBool(params, kPwExternalChargerConnected);
    state.battery_charging_state = csm::getU8(params, kPwBatteryChargingState);
    state.battery_charge_level = csm::getU16(params, kPwBatteryChargeLevel);
    SPDLOG_DEBUG("[iap2] PowerUpdate level={} charger={}", state.battery_charge_level.value_or(0),
                 state.is_external_charger_connected ? (*state.is_external_charger_connected ? 1 : 0) : -1);
    return state;
}

std::vector<uint8_t> encodeStartPowerUpdates()
{
    csm::ParamList params;
    csm::addNone(params, kPwExternalChargerConnected);
    csm::addNone(params, kPwBatteryChargingState);
    csm::addNone(params, kPwBatteryChargeLevel);
    return csm::encodeMessage(kMsgStartPowerUpdates, params);
}

std::vector<uint8_t> encodeStopPowerUpdates() { return csm::encodeMessage(kMsgStopPowerUpdates, {}); }

std::vector<uint8_t> encodePowerSourceUpdate(uint16_t available_current_ma, bool device_battery_should_charge)
{
    csm::ParamList params;
    csm::addU16(params, kPsAvailableCurrentForDevice, available_current_ma);
    csm::addBool(params, kPsBatteryShouldCharge, device_battery_should_charge);
    return csm::encodeMessage(kMsgPowerSourceUpdate, params);
}

// ---------------------------------------------------------------------------
// Communications.
// ---------------------------------------------------------------------------

std::optional<CellularState> decodeCommunicationsUpdate(const csm::ParamList& params)
{
    CellularState state;
    if (const auto signal = csm::getBytes(params, kCommSignalStrength))
    {
        if (!signal->empty())
        {
            state.signal_strength = (*signal)[0];
        }
    }
    state.registration_status = csm::getBytes(params, kCommRegistrationStatus);
    state.airplane_mode = csm::getBool(params, kCommAirplaneMode);
    state.carrier_name = csm::getString(params, kCommCarrierName);
    state.cellular_supported = csm::getBool(params, kCommCellularSupported);
    return state;
}

std::vector<uint8_t> encodeStartCommunicationsUpdates()
{
    csm::ParamList params;
    csm::addNone(params, kCommSignalStrength);
    csm::addNone(params, kCommCarrierName);
    csm::addNone(params, kCommCellularSupported);
    return csm::encodeMessage(kMsgStartCommunicationsUpdates, params);
}

std::vector<uint8_t> encodeStopCommunicationsUpdates()
{
    return csm::encodeMessage(kMsgStopCommunicationsUpdates, {});
}

// ---------------------------------------------------------------------------
// Misc.
// ---------------------------------------------------------------------------

std::vector<uint8_t> encodeVehicleStatusUpdate(std::optional<uint16_t> range,
                                               std::optional<int16_t> outside_temperature,
                                               std::optional<bool> range_warning)
{
    csm::ParamList params;
    if (range)
    {
        csm::addU16(params, kVsRange, *range);
    }
    if (outside_temperature)
    {
        csm::addI16(params, kVsOutsideTemperature, *outside_temperature);
    }
    if (range_warning)
    {
        csm::addBool(params, kVsRangeWarning, *range_warning);
    }
    return csm::encodeMessage(kMsgVehicleStatusUpdate, params);
}

// StartLocationInformation (0xFFFA) parameters -- each is a None-like presence
// flag selecting one NMEA sentence family.
constexpr uint16_t kStartLocGpsFixData = 1;
constexpr uint16_t kStartLocRecommendedMinimum = 2;
constexpr uint16_t kStartLocSatellitesInView = 3;
constexpr uint16_t kStartLocVehicleSpeed = 4;

LocationRequest decodeStartLocationInformation(const csm::ParamList& params)
{
    LocationRequest request;
    request.gps_fix_data = csm::has(params, kStartLocGpsFixData);
    request.recommended_minimum = csm::has(params, kStartLocRecommendedMinimum);
    request.satellites_in_view = csm::has(params, kStartLocSatellitesInView);
    request.vehicle_speed = csm::has(params, kStartLocVehicleSpeed);
    return request;
}

std::vector<uint8_t> encodeLocationInformation(std::string_view nmea_sentence)
{
    csm::ParamList params;
    csm::addString(params, kLocNmeaSentence, nmea_sentence);
    return csm::encodeMessage(kMsgLocationInformation, params);
}

std::vector<uint8_t> encodeAccessoryWiFiConfigurationInformation(std::string_view ssid,
                                                                 std::string_view passphrase,
                                                                 WiFiSecurityType security_type,
                                                                 uint8_t channel)
{
    csm::ParamList params;
    csm::addString(params, kWifiSsid, ssid);
    csm::addString(params, kWifiPassphrase, passphrase);
    csm::addEnum(params, kWifiSecurityType, static_cast<uint8_t>(security_type));
    csm::addU8(params, kWifiChannel, channel);
    return csm::encodeMessage(kMsgAccessoryWiFiConfigurationInformation, params);
}

}  // namespace iap2
