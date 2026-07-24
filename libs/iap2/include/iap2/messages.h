// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/control_session_message/*.py
// and         LIVI src/main/services/projection/driver/bt/cp_handler.py
#ifndef IAP2_MESSAGES_H_
#define IAP2_MESSAGES_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "iap2/mfi_signer.h"

namespace iap2
{

// ---------------------------------------------------------------------------
// Control session message (CSM) framing.
//
//   struct  ">HHH"  = start(0x4040), length(total, incl. this 6 byte header),
//                     message id
//   params  ">HH"   = length(total, incl. this 4 byte header), parameter id
//                     followed by (length - 4) payload bytes
// ---------------------------------------------------------------------------
constexpr uint16_t kCsmStart = 0x4040;
constexpr size_t kCsmHeaderSize = 6;
constexpr size_t kCsmParamHeaderSize = 4;

// ---------------------------------------------------------------------------
// Message identifiers (LIVI control_session_message/*.py @csm(...) decorators).
// ---------------------------------------------------------------------------
enum MessageId : uint16_t
{
    // identification.py
    kMsgStartIdentification = 0x1D00,
    kMsgIdentificationInformation = 0x1D01,
    kMsgIdentificationAccepted = 0x1D02,
    kMsgIdentificationRejected = 0x1D03,

    // authentication.py
    kMsgRequestAuthenticationCertificate = 0xAA00,
    kMsgAuthenticationCertificate = 0xAA01,
    kMsgRequestAuthenticationChallengeResponse = 0xAA02,
    kMsgAuthenticationResponse = 0xAA03,
    kMsgAuthenticationFailed = 0xAA04,
    kMsgAuthenticationSucceeded = 0xAA05,

    // car_play.py
    kMsgCarPlayAvailability = 0x4300,
    kMsgCarPlayStartSession = 0x4301,
    kMsgWirelessCarPlayUpdate = 0x4E0D,
    kMsgDeviceTransportIdentifierNotification = 0x4E0E,

    // communications.py
    kMsgStartCallStateUpdates = 0x4154,
    kMsgCallStateUpdate = 0x4155,
    kMsgStopCallStateUpdates = 0x4156,
    kMsgStartCommunicationsUpdates = 0x4157,
    kMsgCommunicationsUpdate = 0x4158,
    kMsgStopCommunicationsUpdates = 0x4159,

    // now_playing.py
    kMsgStartNowPlayingUpdates = 0x5000,
    kMsgNowPlayingUpdate = 0x5001,
    kMsgStopNowPlayingUpdates = 0x5002,

    // route_guidance.py
    kMsgStartRouteGuidanceUpdates = 0x5200,
    kMsgRouteGuidanceUpdate = 0x5201,
    kMsgRouteGuidanceManeuverUpdate = 0x5202,
    kMsgStopRouteGuidanceUpdates = 0x5203,

    // power.py
    kMsgStartPowerUpdates = 0xAE00,
    kMsgPowerUpdate = 0xAE01,
    kMsgStopPowerUpdates = 0xAE02,
    kMsgPowerSourceUpdate = 0xAE03,

    // vehicle_status.py
    kMsgStartVehicleStatusUpdates = 0xA100,
    kMsgVehicleStatusUpdate = 0xA101,
    kMsgStopVehicleStatusUpdates = 0xA102,

    // location.py
    kMsgStartLocationInformation = 0xFFFA,
    kMsgLocationInformation = 0xFFFB,
    kMsgStopLocationInformation = 0xFFFC,

    // wifi.py
    kMsgRequestWiFiInformation = 0x5700,
    kMsgWiFiInformation = 0x5701,
    kMsgRequestAccessoryWiFiConfigurationInformation = 0x5702,
    kMsgAccessoryWiFiConfigurationInformation = 0x5703,

    // eap.py
    kMsgStartExternalAccessoryProtocolSession = 0xEA00,
    kMsgStopExternalAccessoryProtocolSession = 0xEA01,
    kMsgStatusExternalAccessoryProtocolSession = 0xEA03,
};

// Human readable name for a message id, for logging. Returns "unknown" for
// identifiers we do not model.
const char* messageIdName(uint16_t msg_id);

namespace csm
{

// One serialised parameter. `data` is the parameter payload without the
// 4 byte parameter header.
struct Param
{
    uint16_t id = 0;
    std::vector<uint8_t> data;

    bool operator==(const Param& other) const = default;
};

using ParamList = std::vector<Param>;

// --- encoding helpers -------------------------------------------------------
//
// "None-like" parameters carry no payload at all; on the wire their presence is
// the value (LIVI models them as `NoneLike`). Used by every Start*Updates
// message to select which fields the device should report.
void addNone(ParamList& params, uint16_t id);
void addBool(ParamList& params, uint16_t id, bool value);
void addU8(ParamList& params, uint16_t id, uint8_t value);
void addI8(ParamList& params, uint16_t id, int8_t value);
void addU16(ParamList& params, uint16_t id, uint16_t value);
void addI16(ParamList& params, uint16_t id, int16_t value);
void addU32(ParamList& params, uint16_t id, uint32_t value);
void addU64(ParamList& params, uint16_t id, uint64_t value);
// IntEnum in LIVI is always serialised as a single byte.
void addEnum(ParamList& params, uint16_t id, uint8_t value);
// Strings are UTF-8 with a trailing NUL.
void addString(ParamList& params, uint16_t id, std::string_view value);
void addBytes(ParamList& params, uint16_t id, const std::vector<uint8_t>& value);
// A nested parameter group (LIVI's dataclass-valued parameters).
void addGroup(ParamList& params, uint16_t id, const ParamList& group);

std::vector<uint8_t> encodeParams(const ParamList& params);
std::vector<uint8_t> encodeMessage(uint16_t msg_id, const ParamList& params);

// --- decoding helpers -------------------------------------------------------
struct Message
{
    uint16_t id = 0;
    ParamList params;
};

// Parses the parameter area of a message (everything after the 6 byte header).
std::optional<ParamList> parseParams(const uint8_t* data, size_t len);

// Parses a complete CSM frame (header included). Trailing bytes past the
// declared length are ignored.
std::optional<Message> parseMessage(const uint8_t* data, size_t len);
std::optional<Message> parseMessage(const std::vector<uint8_t>& frame);

// If `data` holds a complete CSM header, returns the total frame length.
// Returns nullopt when there are fewer than 6 bytes; returns 0 when the header
// is present but malformed (bad start marker or a length below the header).
std::optional<size_t> peekLength(const uint8_t* data, size_t len);

const Param* find(const ParamList& params, uint16_t id);
bool has(const ParamList& params, uint16_t id);

std::optional<bool> getBool(const ParamList& params, uint16_t id);
std::optional<uint8_t> getU8(const ParamList& params, uint16_t id);
std::optional<int8_t> getI8(const ParamList& params, uint16_t id);
std::optional<uint16_t> getU16(const ParamList& params, uint16_t id);
std::optional<int16_t> getI16(const ParamList& params, uint16_t id);
std::optional<uint32_t> getU32(const ParamList& params, uint16_t id);
std::optional<uint64_t> getU64(const ParamList& params, uint16_t id);
std::optional<std::string> getString(const ParamList& params, uint16_t id);
std::optional<std::vector<uint8_t>> getBytes(const ParamList& params, uint16_t id);
std::optional<ParamList> getGroup(const ParamList& params, uint16_t id);

}  // namespace csm

// ---------------------------------------------------------------------------
// Enumerations (LIVI IntEnums).
// ---------------------------------------------------------------------------
enum class PowerProvidingCapability : uint8_t
{
    kNone = 0,
    kReserved = 1,
    kAdvanced = 2,
};

enum class MatchAction : uint8_t
{
    kNone = 0,
    kSettingsAndPrompt = 1,
    kSettingsOnly = 2,
};

enum class EngineType : uint8_t
{
    kGas = 0,
    kDiesel = 1,
    kElectric = 2,
    kCng = 3,
};

enum class PlaybackStatus : uint8_t
{
    kStopped = 0,
    kPlaying = 1,
    kPaused = 2,
    kSeekForward = 3,
    kSeekBackward = 4,
};

enum class WirelessCarPlayStatus : uint8_t
{
    kUnavailable = 0,
    kAvailable = 1,
};

enum class WiFiSecurityType : uint8_t
{
    kNone = 0,
    kWep = 1,
    kWpaWpa2 = 2,
    kWpa3Transition = 3,
    kWpa3Only = 4,
};

// ---------------------------------------------------------------------------
// IdentificationInformation (0x1D01).
//
// The defaults describe the wired ("carkit") accessory LIVI advertises:
// a single USBHostTransportComponent with CarPlay interface 3 and
// PowerProvidingCapability::ADVANCED.
// ---------------------------------------------------------------------------
struct IdentificationConfig
{
    std::string name = "LIVI";
    std::string model_identifier = "LIVI";
    std::string manufacturer = "LIVI";
    std::string serial_number = "0123456";
    std::string firmware_version = "1.0.0";
    std::string hardware_version = "1.0";

    PowerProvidingCapability power_providing_capability = PowerProvidingCapability::kAdvanced;
    uint16_t maximum_current_drawn_from_device = 20;

    // External accessory protocol advertised to the phone.
    uint8_t ea_protocol_id = 1;
    std::string ea_protocol_name = "en.opencarplay.test";
    MatchAction ea_match_action = MatchAction::kNone;

    std::string current_language = "en";
    std::vector<std::string> supported_languages = {"en", "de"};

    // USBHostTransportComponent (wired CarPlay).
    uint16_t usb_host_component_id = 0;
    std::string usb_host_component_name = "USBHostTransport";
    uint8_t car_play_interface_number = 3;
    bool supports_car_play = true;

    // We ask the phone to hand us the AirPlay endpoint via CarPlayStartSession
    // rather than discovering it over Bonjour. Adds CarPlayStartSession /
    // CarPlayAvailability to the advertised message lists.
    bool carplay_wired_start_session = true;

    // Optional components. The phone may reject identification because of any
    // of these; clear the offending one and re-send (see
    // IdentificationRejection::droppable()).
    EngineType engine_type = EngineType::kDiesel;
    bool include_vehicle_information = true;
    bool include_vehicle_status = true;
    bool include_location_information = true;
    bool include_route_guidance_display = true;
};

std::vector<uint8_t> encodeIdentificationInformation(const IdentificationConfig& config);

// The message id lists advertised inside IdentificationInformation. Exposed so
// tests (and callers) can check what we claim to speak.
std::vector<uint16_t> identificationMessagesSent(const IdentificationConfig& config);
std::vector<uint16_t> identificationMessagesReceived(const IdentificationConfig& config);

// IdentificationRejected (0x1D03): every parameter is None-like, present means
// "the phone objected to this field".
struct IdentificationRejection
{
    std::vector<uint16_t> flagged_params;
    std::vector<std::string> flagged_names;

    bool contains(uint16_t param_id) const;
};

std::optional<IdentificationRejection> decodeIdentificationRejected(const csm::ParamList& params);

// Clears whichever optional component the phone flagged. Returns false when the
// rejection names nothing we are willing to drop (identification has failed).
bool applyIdentificationRejection(const IdentificationRejection& rejection, IdentificationConfig& config);

// ---------------------------------------------------------------------------
// Authentication (0xAA00 - 0xAA05).
// ---------------------------------------------------------------------------
std::vector<uint8_t> encodeAuthenticationCertificate(const std::vector<uint8_t>& certificate);
std::vector<uint8_t> encodeAuthenticationResponse(const std::vector<uint8_t>& response);
std::optional<std::vector<uint8_t>> decodeAuthenticationChallenge(const csm::ParamList& params);

// Drives the MFi side of the iAP2 authentication handshake.
class MfiAuthenticator
{
  public:
    enum class Result
    {
        kIgnored,    // not an authentication message
        kReply,      // `reply` holds a CSM frame that must be sent
        kSucceeded,  // device accepted us
        kFailed,     // device rejected us, or the coprocessor failed
    };

    explicit MfiAuthenticator(MfiSigner& signer);

    // Feed every inbound control session message here.
    Result handle(const csm::Message& message, std::vector<uint8_t>& reply);

  private:
    MfiSigner& signer_;
};

// ---------------------------------------------------------------------------
// CarPlay session control (0x4300 / 0x4301).
// ---------------------------------------------------------------------------
struct CarPlayAvailability
{
    bool has_wired = false;
    std::optional<bool> wired_available;
    std::optional<std::string> usb_transport_identifier;

    bool has_wireless = false;
    std::optional<bool> wireless_available;
    std::optional<std::string> bluetooth_transport_identifier;
};

std::optional<CarPlayAvailability> decodeCarPlayAvailability(const csm::ParamList& params);

struct CarPlayStartSession
{
    // Accessory link-local IPv6 address of the USB ethernet (NCM) interface,
    // e.g. "fe80::1c2:3ff:fe45:6789". LIVI sends exactly one.
    std::vector<std::string> ip_addresses;
    uint32_t port = 7000;
    // The accessory Bluetooth MAC, colon separated ("aa:bb:cc:dd:ee:ff").
    std::string device_identifier;
    // AirPlay 2 public key ("pi"), as advertised over Bonjour.
    std::string public_key;
    std::string source_version = "410.35";
};

std::vector<uint8_t> encodeCarPlayStartSession(const CarPlayStartSession& session);

struct DeviceTransportIdentifiers
{
    std::optional<std::string> bluetooth_transport_id;
    std::optional<std::string> usb_transport_id;
};

std::optional<DeviceTransportIdentifiers> decodeDeviceTransportIdentifierNotification(const csm::ParamList& params);
std::optional<WirelessCarPlayStatus> decodeWirelessCarPlayUpdate(const csm::ParamList& params);

// ---------------------------------------------------------------------------
// Now playing (0x5000 / 0x5001).
// ---------------------------------------------------------------------------
struct NowPlaying
{
    bool has_media_item = false;
    std::optional<uint64_t> persistent_id;
    std::optional<std::string> title;
    std::optional<uint32_t> duration_ms;
    std::optional<std::string> album;
    std::optional<std::string> artist;
    std::optional<std::string> album_artist;
    std::optional<std::string> genre;
    // File transfer identifier the artwork will arrive on, if offered.
    std::optional<uint8_t> artwork_ftid;

    bool has_playback = false;
    std::optional<PlaybackStatus> status;
    std::optional<uint32_t> elapsed_ms;
    std::optional<std::string> app_name;
    std::optional<std::string> app_bundle_id;
};

std::optional<NowPlaying> decodeNowPlayingUpdate(const csm::ParamList& params);
// Subscribes to exactly the fields LIVI asks for.
std::vector<uint8_t> encodeStartNowPlayingUpdates();
std::vector<uint8_t> encodeStopNowPlayingUpdates();

// ---------------------------------------------------------------------------
// Route guidance (0x5200 - 0x5203).
// ---------------------------------------------------------------------------
struct RouteGuidance
{
    std::optional<uint16_t> display_component_id;
    std::optional<uint8_t> state;
    std::optional<uint8_t> maneuver_state;
    std::optional<std::string> current_road_name;
    std::optional<std::string> destination_name;
    std::optional<uint64_t> eta;
    std::optional<uint64_t> time_remaining;
    std::optional<uint32_t> distance_remaining;
    std::optional<uint32_t> distance_to_maneuver;
    // Big-endian list of maneuver indices; the first entry is the maneuver
    // currently being executed.
    std::optional<std::vector<uint8_t>> current_maneuver_list;
};

struct RouteManeuver
{
    std::optional<uint16_t> display_component_id;
    std::optional<uint16_t> index;
    std::optional<uint8_t> maneuver_type;
    std::optional<std::string> after_maneuver_road_name;
    std::optional<uint8_t> driving_side;
    std::optional<uint8_t> junction_type;
    std::optional<int16_t> exit_angle;
};

std::optional<RouteGuidance> decodeRouteGuidanceUpdate(const csm::ParamList& params);
std::optional<RouteManeuver> decodeRouteGuidanceManeuverUpdate(const csm::ParamList& params);
std::vector<uint8_t> encodeStartRouteGuidanceUpdates();
std::vector<uint8_t> encodeStopRouteGuidanceUpdates();

// The merged navigation state LIVI publishes (cp_handler _handle_route_guidance
// / _handle_route_maneuver / _push_route_guidance). Guidance updates and
// maneuver updates arrive separately and are folded together here.
struct NavGuidance
{
    std::optional<uint8_t> status;      // RouteGuidance::state
    std::optional<uint8_t> order_type;  // RouteGuidance::maneuver_state
    std::optional<std::string> road_name;
    std::optional<std::string> destination_name;
    std::optional<uint64_t> eta_epoch;
    std::optional<uint64_t> time_to_destination;
    std::optional<uint32_t> distance_to_destination;
    std::optional<uint32_t> remain_distance;

    // Fields of the maneuver currently being executed.
    std::optional<uint16_t> current_index;
    std::optional<uint8_t> maneuver_type;
    std::optional<uint8_t> turn_side;
    std::optional<uint8_t> junction_type;
    std::optional<int16_t> turn_angle;
    std::optional<std::string> after_road_name;

    void apply(const RouteGuidance& update);
    void apply(const RouteManeuver& update);

  private:
    struct Maneuver
    {
        std::optional<uint8_t> maneuver_type;
        std::optional<uint8_t> turn_side;
        std::optional<uint8_t> junction_type;
        std::optional<int16_t> turn_angle;
        std::optional<std::string> after_road_name;
    };

    void refreshCurrent();

    std::map<uint16_t, Maneuver> maneuvers_;
};

// ---------------------------------------------------------------------------
// Call state (0x4154 / 0x4155).
// ---------------------------------------------------------------------------
struct CallState
{
    std::optional<std::string> remote_id;
    std::optional<std::string> display_name;
    std::optional<uint8_t> status;
    std::optional<uint8_t> direction;
    std::optional<std::string> call_uuid;
    std::optional<uint8_t> disconnect_reason;
};

std::optional<CallState> decodeCallStateUpdate(const csm::ParamList& params);
std::vector<uint8_t> encodeStartCallStateUpdates();
std::vector<uint8_t> encodeStopCallStateUpdates();

// Folds per-call updates into the single "phase" LIVI publishes.
class CallTracker
{
  public:
    enum class Phase
    {
        kEnded,
        kRinging,
        kActive,
    };

    // Returns true when the phase changed as a result of this update.
    bool apply(const CallState& update);

    Phase phase() const { return phase_; }
    const std::string& number() const { return number_; }
    const std::string& name() const { return name_; }

    static const char* phaseName(Phase phase);

  private:
    struct Call
    {
        uint8_t status = 0;
        std::string number;
        std::string name;
    };

    std::map<std::string, Call> calls_;
    Phase phase_ = Phase::kEnded;
    std::string number_;
    std::string name_;
};

// ---------------------------------------------------------------------------
// Power (0xAE00 - 0xAE03).
// ---------------------------------------------------------------------------
struct PowerState
{
    std::optional<uint16_t> maximum_current_drawn_from_accessory;
    std::optional<bool> device_battery_will_charge_if_power_is_present;
    std::optional<uint8_t> accessory_power_mode;
    std::optional<bool> is_external_charger_connected;
    std::optional<uint8_t> battery_charging_state;
    std::optional<uint16_t> battery_charge_level;
};

std::optional<PowerState> decodePowerUpdate(const csm::ParamList& params);
std::vector<uint8_t> encodeStartPowerUpdates();
std::vector<uint8_t> encodeStopPowerUpdates();
// Tells the phone how much current it may draw from us.
std::vector<uint8_t> encodePowerSourceUpdate(uint16_t available_current_ma, bool device_battery_should_charge);

// ---------------------------------------------------------------------------
// Communications / cellular (0x4157 / 0x4158).
// ---------------------------------------------------------------------------
struct CellularState
{
    std::optional<uint8_t> signal_strength;
    std::optional<std::vector<uint8_t>> registration_status;
    std::optional<bool> airplane_mode;
    std::optional<std::string> carrier_name;
    std::optional<bool> cellular_supported;
};

std::optional<CellularState> decodeCommunicationsUpdate(const csm::ParamList& params);
std::vector<uint8_t> encodeStartCommunicationsUpdates();
std::vector<uint8_t> encodeStopCommunicationsUpdates();

// ---------------------------------------------------------------------------
// Misc accessory -> device messages LIVI sends.
// ---------------------------------------------------------------------------
std::vector<uint8_t> encodeVehicleStatusUpdate(std::optional<uint16_t> range,
                                               std::optional<int16_t> outside_temperature,
                                               std::optional<bool> range_warning);
// Which NMEA sentence families the phone asked for in StartLocationInformation
// (0xFFFA). Each is a presence flag in the request.
struct LocationRequest
{
    bool gps_fix_data = false;        // -> $GPGGA
    bool recommended_minimum = false; // -> $GPRMC
    bool satellites_in_view = false;  // -> $GPGSV (not generated yet)
    bool vehicle_speed = false;       // -> $GPVTG (not generated yet)

    bool any() const
    {
        return gps_fix_data || recommended_minimum || satellites_in_view || vehicle_speed;
    }
};

LocationRequest decodeStartLocationInformation(const csm::ParamList& params);

std::vector<uint8_t> encodeLocationInformation(std::string_view nmea_sentence);
std::vector<uint8_t> encodeAccessoryWiFiConfigurationInformation(std::string_view ssid,
                                                                 std::string_view passphrase,
                                                                 WiFiSecurityType security_type,
                                                                 uint8_t channel);

}  // namespace iap2

#endif  // IAP2_MESSAGES_H_
