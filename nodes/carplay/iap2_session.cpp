// SPDX-License-Identifier: GPL-3.0-or-later
#include "iap2_session.h"

#include "start_session_gate.h"

#include "iap2/link_layer.h"
#include "iap2/file_transfer.h"
#include "iap2/messages.h"

#include <spdlog/spdlog.h>
#include <spdlog/fmt/ranges.h>

#include <chrono>
#include <map>
#include <memory>

namespace carplay
{
namespace
{

// ---------------------------------------------------------------------------
// Adapts the lockdown/carkit TLS channel to the link layer's transport
// interface. The shapes already match; the reason this is not a plain cast is
// liveness: the link layer reads an empty recv() as "no data yet", so a dead
// channel has to be surfaced explicitly or poll() spins forever on a corpse.
// ---------------------------------------------------------------------------
class CarkitTransport : public iap2::Iap2Transport
{
  public:
    explicit CarkitTransport(apple_usb::CarkitChannel& channel) : channel_(channel) {}

    bool send(const uint8_t* data, size_t len) override { return channel_.send(data, len); }

    std::vector<uint8_t> recv(size_t max_len, unsigned timeout_ms) override
    {
        return channel_.recv(max_len, timeout_ms);
    }

    bool alive() const { return channel_.alive(); }

  private:
    apple_usb::CarkitChannel& channel_;
};

const char* stateName(iap2::LinkLayer::State state)
{
    switch (state)
    {
        case iap2::LinkLayer::State::kIdle: return "idle";
        case iap2::LinkLayer::State::kDetectIap2Support: return "detect";
        case iap2::LinkLayer::State::kNegotiate: return "negotiate";
        case iap2::LinkLayer::State::kNormal: return "normal";
        case iap2::LinkLayer::State::kDead: return "dead";
    }
    return "?";
}

// CarPlayAvailability carries wired_available as a boolean that a phone may
// send zero-length. csm::getBool() reports that as absent and the session is
// started anyway (CarPlayAvailability::permitsWiredStart), but it is logged so
// the case is visible if it ever turns up.
void logAvailability(const iap2::csm::Message& message, const iap2::CarPlayAvailability& availability)
{
    const iap2::csm::Param* wired = nullptr;
    for (const auto& param : message.params)
    {
        // 0x0000 is the wired-availability parameter inside CarPlayAvailability.
        if (param.id == 0x0000)
        {
            wired = &param;
            break;
        }
    }

    if (wired != nullptr && wired->data.empty())
    {
        SPDLOG_INFO("[iap2] CarPlayAvailability wired parameter is zero length; read as "
                    "available");
    }

    SPDLOG_INFO("[iap2] CarPlayAvailability: has_wired={} wired_available={} usb_transport_id={}",
                availability.has_wired,
                availability.wired_available
                    ? (*availability.wired_available ? "true" : "false")
                    : "<absent>",
                availability.usb_transport_identifier.value_or("<none>"));
}

}  // namespace

bool runIap2Session(apple_usb::CarkitChannel& channel, const Iap2SessionOptions& options,
                    std::atomic<bool>& stop)
{
    CarkitTransport transport(channel);

    // Wired carkit defaults: zero-ack, control session version 2, and we drive
    // the negotiation rather than waiting for the phone's detection marker.
    iap2::LinkConfig config;
    config.tag = "carkit";

    iap2::LinkLayer link(transport, config);

    // --- MFi coprocessor -----------------------------------------------------
    // The caller's, shared with AirPlay /auth-setup, and null when it is not up
    // (the caller retries opening it every session).
    iap2::MfiSigner* signer = options.signer;
    std::unique_ptr<iap2::MfiAuthenticator> authenticator;
    if (signer != nullptr)
    {
        SPDLOG_INFO("[mfi] using the shared coprocessor, protocol major {}",
                    signer->protocolMajor());
        authenticator = std::make_unique<iap2::MfiAuthenticator>(*signer);
    }
    else if (options.allow_missing_mfi)
    {
        SPDLOG_WARN("[mfi] coprocessor unavailable -- continuing anyway as requested. "
                    "The phone will refuse CarPlay, but the link layer and "
                    "identification are still exercised.");
    }
    else
    {
        SPDLOG_ERROR("[mfi] coprocessor unavailable. Verify it standalone with "
                     "./build/libs/apple_mfi_ic/apple_mfi_demo, or pass "
                     "--iap2-allow-missing-mfi to continue without it.");
        return false;
    }

    iap2::IdentificationConfig identification;
    if (options.identity)
    {
        // Left unset, IdentificationConfig still identifies the accessory as
        // the project this was ported from, with its serial number.
        const VehicleIdentity& vehicle = *options.identity;
        identification.name = vehicle.name;
        identification.model_identifier = vehicle.model;
        identification.manufacturer = vehicle.manufacturer;
        identification.serial_number = vehicle.serial_number;
        identification.firmware_version = vehicle.firmware_version;
        identification.hardware_version = vehicle.hardware_version;
        identification.engine_type = vehicle.engine_type;
        identification.current_language = vehicle.language;
        identification.supported_languages = vehicle.supported_languages;
        // Only claim a VehicleStatusComponent when we can actually answer the
        // subscription that follows it. The phone sends
        // StartVehicleStatusUpdates on every session; advertising range and
        // outside temperature and then never sending an update is a promise
        // broken every time.
        identification.include_vehicle_status =
            options.vehicle_status != nullptr && options.vehicle_status->advertised();
        SPDLOG_INFO("[iap2] vehicle status {}",
                    identification.include_vehicle_status
                        ? "advertised (configured or live)"
                        : "not advertised (nothing configured, no live source)");
        SPDLOG_INFO("[iap2] identifying as {} / {} ({}), serial {}", identification.manufacturer,
                    identification.model_identifier, identification.name,
                    identification.serial_number);
    }
    bool identified = false;
    bool vehicle_status_active = false;
    bool authenticated = false;
    bool session_started = false;
    bool failed = false;

    // Navigation and call state are stateful: route-guidance and maneuver
    // updates arrive separately and merge into one NavGuidance, and per-call
    // updates fold into a single CallTracker phase. Held across callbacks.
    iap2::NavGuidance nav_state;
    iap2::CallTracker call_tracker;

    // The phone's standing location request (which NMEA families it wants).
    // Set/cleared by Start/StopLocationInformation, serviced from the poll loop.
    iap2::LocationRequest location_request;
    auto last_location_send = std::chrono::steady_clock::now();

    // Sends the vehicle status if it changed since the phone was last told. The
    // values may be live, so this is polled as well as called on subscription.
    const auto send_vehicle_status = [&] {
        if (!vehicle_status_active || options.vehicle_status == nullptr)
        {
            return;
        }
        const auto status = options.vehicle_status->takeChange();
        if (!status)
        {
            return;
        }
        SPDLOG_INFO("[iap2] vehicle status: range={} temp={} warning={}",
                    status->range_km ? std::to_string(*status->range_km) : "unset",
                    status->outside_temperature_c ? std::to_string(*status->outside_temperature_c)
                                                  : "unset",
                    status->range_warning ? (*status->range_warning ? "true" : "false") : "unset");
        link.sendControlMessage(iap2::encodeVehicleStatusUpdate(
            status->range_km, status->outside_temperature_c, status->range_warning));
    };

    // CarPlayStartSession needs the NCM link-local, which may not exist yet when
    // the phone asks: see StartSessionGate. Called from the handler below and
    // then from the poll loop until it sends or gives up.
    StartSessionGate start_gate(options.start_session_patience);
    bool start_abandoned = false;
    const auto service_start_session = [&] {
        if (!start_gate.pending())
        {
            return;
        }
        const auto endpoint = options.endpoint_provider();
        switch (start_gate.poll(std::chrono::steady_clock::now(), endpoint.has_value()))
        {
            case StartSessionGate::Action::kNone:
                break;
            case StartSessionGate::Action::kSend:
            {
                iap2::CarPlayStartSession session;
                session.ip_addresses = {endpoint->link_local_address};
                session.port = endpoint->port;
                session.device_identifier = endpoint->device_identifier;
                session.public_key = endpoint->public_key;

                SPDLOG_INFO("[iap2] sending CarPlayStartSession -> [{}]:{} id={}",
                            endpoint->link_local_address, endpoint->port,
                            endpoint->device_identifier);
                link.sendControlMessage(iap2::encodeCarPlayStartSession(session));
                session_started = true;
                break;
            }
            case StartSessionGate::Action::kGiveUp:
                SPDLOG_ERROR("[iap2] the NCM link never got an address, so there is nothing to "
                             "hand the phone -- CarPlayStartSession not sent; ending the session "
                             "so the bring-up is retried");
                start_abandoned = true;
                break;
        }
    };

    link.setControlMessageHandler([&](const std::vector<uint8_t>& frame) {
        const auto message = iap2::csm::parseMessage(frame);
        if (!message)
        {
            SPDLOG_WARN("[iap2] undecodable control message ({} bytes)", frame.size());
            return;
        }

        SPDLOG_DEBUG("[iap2] <- {} (0x{:04x}), {} param(s)",
                     iap2::messageIdName(message->id), message->id, message->params.size());

        switch (message->id)
        {
            case iap2::kMsgStartIdentification:
            {
                SPDLOG_INFO("[iap2] phone requested identification");
                const auto frame_out = iap2::encodeIdentificationInformation(identification);
                link.sendControlMessage(frame_out);
                break;
            }

            case iap2::kMsgIdentificationAccepted:
                SPDLOG_INFO("[iap2] identification ACCEPTED");
                identified = true;
                break;

            case iap2::kMsgIdentificationRejected:
            {
                const auto rejection = iap2::decodeIdentificationRejected(message->params);
                if (!rejection)
                {
                    SPDLOG_ERROR("[iap2] identification rejected, reason undecodable");
                    failed = true;
                    break;
                }
                SPDLOG_WARN("[iap2] identification REJECTED, phone flagged: {}",
                            fmt::join(rejection->flagged_names, ", "));
                if (!iap2::applyIdentificationRejection(*rejection, identification))
                {
                    SPDLOG_ERROR("[iap2] the rejected field is not one we can drop -- "
                                 "identification has failed");
                    failed = true;
                    break;
                }
                SPDLOG_INFO("[iap2] dropped the flagged component, re-sending identification");
                link.sendControlMessage(iap2::encodeIdentificationInformation(identification));
                break;
            }

            case iap2::kMsgRequestAuthenticationCertificate:
            case iap2::kMsgRequestAuthenticationChallengeResponse:
            case iap2::kMsgAuthenticationFailed:
            case iap2::kMsgAuthenticationSucceeded:
            {
                if (!authenticator)
                {
                    SPDLOG_ERROR("[mfi] phone asked for authentication ({}) but no coprocessor "
                                 "is available -- CarPlay cannot start.",
                                 iap2::messageIdName(message->id));
                    failed = true;
                    break;
                }
                std::vector<uint8_t> reply;
                switch (authenticator->handle(*message, reply))
                {
                    case iap2::MfiAuthenticator::Result::kReply:
                        SPDLOG_INFO("[mfi] answering {}", iap2::messageIdName(message->id));
                        link.sendControlMessage(reply);
                        break;
                    case iap2::MfiAuthenticator::Result::kSucceeded:
                        SPDLOG_INFO("[mfi] authentication SUCCEEDED");
                        authenticated = true;
                        SPDLOG_INFO("[iap2] subscribing to now-playing, navigation, call updates; "
                                    "offering {} mA", options.available_current_ma);
                        for (const auto& subscription :
                             iap2::encodeAfterAuthentication(options.available_current_ma))
                        {
                            link.sendControlMessage(subscription);
                        }
                        break;
                    case iap2::MfiAuthenticator::Result::kFailed:
                        SPDLOG_ERROR("[mfi] authentication FAILED. Check the protocol major "
                                     "(2 => SHA-1/20B, 3 => SHA-256/32B).");
                        failed = true;
                        break;
                    case iap2::MfiAuthenticator::Result::kIgnored:
                        break;
                }
                break;
            }

            case iap2::kMsgCarPlayAvailability:
            {
                const auto availability = iap2::decodeCarPlayAvailability(message->params);
                if (!availability)
                {
                    SPDLOG_WARN("[iap2] CarPlayAvailability did not decode");
                    break;
                }
                logAvailability(*message, *availability);

                if (availability->permitsWiredStart())
                {
                    SPDLOG_INFO("[iap2] phone reports wired CarPlay AVAILABLE");

                    if (!options.endpoint_provider)
                    {
                        SPDLOG_WARN("[iap2] no accessory endpoint available, so "
                                    "CarPlayStartSession will not be sent (stage 6 not run)");
                        break;
                    }
                    start_gate.request(std::chrono::steady_clock::now());
                    service_start_session();
                    if (start_gate.pending())
                    {
                        SPDLOG_WARN("[iap2] the NCM link has no address yet, so CarPlayStartSession "
                                    "is held until it does (up to {} s)",
                                    options.start_session_patience.count() / 1000);
                    }
                }
                else
                {
                    SPDLOG_WARN("[iap2] wired CarPlay reported unavailable -- not starting a "
                                "session.");
                }
                break;
            }

            case iap2::kMsgNowPlayingUpdate:
            {
                const auto now_playing = iap2::decodeNowPlayingUpdate(message->params);
                if (!now_playing)
                {
                    SPDLOG_DEBUG("[iap2] NowPlayingUpdate did not decode");
                    break;
                }
                // The phone sends these ~2/s even when nothing changed, and each
                // is partial (title absent on an elapsed-only tick), so this
                // stays at DEBUG. The pipeline logs the merged state at INFO only
                // when it actually changes.
                SPDLOG_DEBUG("[iap2] now playing update: '{}' / '{}' ({})",
                             now_playing->title.value_or("?"), now_playing->artist.value_or("?"),
                             now_playing->status.has_value() &&
                                     *now_playing->status == iap2::PlaybackStatus::kPlaying
                                 ? "playing"
                                 : "paused");
                if (options.now_playing_handler)
                {
                    options.now_playing_handler(*now_playing);
                }
                break;
            }

            case iap2::kMsgRouteGuidanceUpdate:
            {
                const auto guidance = iap2::decodeRouteGuidanceUpdate(message->params);
                if (!guidance)
                {
                    SPDLOG_DEBUG("[iap2] RouteGuidanceUpdate did not decode");
                    break;
                }
                nav_state.apply(*guidance);
                // state values seen on hardware: 0 = not routing, 1 = actively
                // guiding (destination present), 3 = transient (calculating).
                SPDLOG_DEBUG("[iap2] navigation: state={} road '{}' -> '{}'",
                             guidance->state.has_value() ? std::to_string(*guidance->state)
                                                         : "none",
                             nav_state.road_name.value_or("?"),
                             nav_state.destination_name.value_or("?"));
                if (options.nav_handler)
                {
                    options.nav_handler(nav_state);
                }
                break;
            }

            case iap2::kMsgRouteGuidanceManeuverUpdate:
            {
                const auto maneuver = iap2::decodeRouteGuidanceManeuverUpdate(message->params);
                if (!maneuver)
                {
                    SPDLOG_DEBUG("[iap2] RouteGuidanceManeuverUpdate did not decode");
                    break;
                }
                nav_state.apply(*maneuver);
                if (options.nav_handler)
                {
                    options.nav_handler(nav_state);
                }
                break;
            }

            case iap2::kMsgStartLocationInformation:
            {
                location_request = iap2::decodeStartLocationInformation(message->params);
                SPDLOG_INFO("[iap2] location requested: GGA={} RMC={} GSV={} VTG={}",
                            location_request.gps_fix_data, location_request.recommended_minimum,
                            location_request.satellites_in_view, location_request.vehicle_speed);
                if (!options.location_provider)
                {
                    SPDLOG_WARN("[iap2] phone asked for GPS location but no location source is "
                                "wired up; ignoring");
                }
                // Force an immediate send on the next poll.
                last_location_send = std::chrono::steady_clock::now() - std::chrono::seconds(2);
                break;
            }

            case iap2::kMsgStopLocationInformation:
                SPDLOG_INFO("[iap2] location updates stopped");
                location_request = {};
                break;

            case iap2::kMsgCallStateUpdate:
            {
                const auto call = iap2::decodeCallStateUpdate(message->params);
                if (!call)
                {
                    SPDLOG_DEBUG("[iap2] CallStateUpdate did not decode");
                    break;
                }
                if (call_tracker.apply(*call))
                {
                    SPDLOG_INFO("[iap2] call: {} ('{}' / '{}')",
                                iap2::CallTracker::phaseName(call_tracker.phase()),
                                call_tracker.name(), call_tracker.number());
                }
                if (options.call_handler)
                {
                    options.call_handler(call_tracker);
                }
                break;
            }

            case iap2::kMsgStartVehicleStatusUpdates:
            {
                // The phone subscribing. It wants the current values now, and
                // then again whenever they change; the poll loop sends those.
                vehicle_status_active = true;
                if (options.vehicle_status == nullptr)
                {
                    SPDLOG_WARN("[iap2] phone subscribed to vehicle status but none is "
                                "configured; nothing to send");
                    break;
                }
                options.vehicle_status->subscribed();
                send_vehicle_status();
                break;
            }

            case iap2::kMsgStopVehicleStatusUpdates:
                vehicle_status_active = false;
                SPDLOG_INFO("[iap2] vehicle status updates stopped");
                break;

            case iap2::kMsgWirelessCarPlayUpdate:
            {
                const auto status = iap2::decodeWirelessCarPlayUpdate(message->params);
                SPDLOG_INFO("[iap2] wireless CarPlay is {} on the phone (we are wired; nothing "
                            "to do)",
                            status && *status == iap2::WirelessCarPlayStatus::kAvailable
                                ? "available"
                                : "unavailable");
                break;
            }

            case iap2::kMsgPowerUpdate:
            {
                if (const auto power = iap2::decodePowerUpdate(message->params);
                    power && options.power_handler)
                {
                    options.power_handler(*power);
                }
                break;
            }

            case iap2::kMsgCommunicationsUpdate:
            {
                if (const auto comms = iap2::decodeCommunicationsUpdate(message->params);
                    comms && options.communications_handler)
                {
                    options.communications_handler(*comms);
                }
                break;
            }

            case iap2::kMsgDeviceTimeUpdate:
            {
                const auto time = iap2::decodeDeviceTimeUpdate(message->params);
                if (!time)
                {
                    break;
                }
                SPDLOG_INFO("[iap2] phone time zone: UTC{:+} min ({} min of it daylight saving)",
                            time->utc_offset_minutes.value_or(0),
                            time->dst_offset_minutes.value_or(0));
                if (options.device_time_handler)
                {
                    options.device_time_handler(*time);
                }
                break;
            }

            case iap2::kMsgDeviceTransportIdentifierNotification:
            {
                // The phone naming the transports it can be reached on. Only
                // useful for handing a session over to Bluetooth or Wi-Fi,
                // which this accessory does not do.
                const auto ids =
                    iap2::decodeDeviceTransportIdentifierNotification(message->params);
                SPDLOG_INFO("[iap2] device transport identifiers: bluetooth={} usb={}",
                            ids && ids->bluetooth_transport_id ? *ids->bluetooth_transport_id
                                                               : "none",
                            ids && ids->usb_transport_id ? *ids->usb_transport_id : "none");
                break;
            }

            case iap2::kMsgRequestAccessoryWiFiConfigurationInformation:
                // The first step of the handover to wireless CarPlay: the phone
                // wants our Wi-Fi credentials so it can join and continue
                // there. Deliberately unanswered -- this is a wired accessory
                // with no Wi-Fi to offer, and an empty answer is not obviously
                // better than none. The phone carries on over USB regardless,
                // which is what we want.
                SPDLOG_INFO("[iap2] phone asked for our Wi-Fi configuration (wireless CarPlay "
                            "handover); declining, this accessory is wired only");
                break;

            default:
                SPDLOG_DEBUG("[iap2] unhandled {} (0x{:04x})",
                             iap2::messageIdName(message->id), message->id);
                break;
        }
    });

    // File-transfer receiver for album artwork. The phone pushes it on the
    // file-transfer session after a track change: SETUP announces a transfer id
    // (we ack with START), then data chunks arrive, and a final chunk completes
    // it (we ack with SUCCESS). Per-ftid buffers persist across callbacks.
    auto transfers = std::make_shared<iap2::FileTransferAssembler>();
    link.setFileTransferHandler([&link, &options, transfers](const std::vector<uint8_t>& dgram) {
        auto result = transfers->handle(dgram);
        for (const auto& reply : result.replies)
        {
            link.sendFileTransfer(reply);
        }
        if (result.completed)
        {
            SPDLOG_INFO("[iap2] album artwork received: {} bytes", result.completed->size());
            if (options.artwork_handler)
            {
                options.artwork_handler(*result.completed);
            }
        }
    });

    if (!link.start())
    {
        SPDLOG_ERROR("[iap2] link start failed");
        return false;
    }
    SPDLOG_INFO("[iap2] link started, negotiating");

    if (!link.waitNegotiated(options.negotiate_timeout_ms))
    {
        SPDLOG_ERROR("[iap2] link did not negotiate within {} ms (state {}). {}",
                     options.negotiate_timeout_ms, stateName(link.state()),
                     transport.alive() ? "The channel is still alive, so suspect SYN/ACK "
                                         "handling."
                                       : "The carkit channel died -- suspect stage 4.");
        return false;
    }
    SPDLOG_INFO("[iap2] link NEGOTIATED (SYN/ACK complete)");

    // LIVI sends identification unprompted rather than waiting for
    // StartIdentification; the phone accepts either order.
    SPDLOG_INFO("[iap2] sending IdentificationInformation");
    link.sendControlMessage(iap2::encodeIdentificationInformation(identification));

    while (!stop.load() && !failed && !start_abandoned)
    {
        service_start_session();
        if (!link.poll(200))
        {
            SPDLOG_ERROR("[iap2] link died (state {})", stateName(link.state()));
            break;
        }
        if (!transport.alive())
        {
            SPDLOG_ERROR("[iap2] carkit channel died underneath the link layer");
            break;
        }

        send_vehicle_status();

        // While the phone wants location, feed it ~1 Hz. Each requested NMEA
        // family goes in its own LocationInformation message.
        if (location_request.any() && options.location_provider)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_location_send >= std::chrono::seconds(1))
            {
                last_location_send = now;
                if (const auto fix = options.location_provider(); fix)
                {
                    if (location_request.gps_fix_data)
                    {
                        link.sendControlMessage(
                            iap2::encodeLocationInformation(iap2::nmeaGga(*fix)));
                    }
                    if (location_request.recommended_minimum)
                    {
                        link.sendControlMessage(
                            iap2::encodeLocationInformation(iap2::nmeaRmc(*fix)));
                    }
                    // GSV / VTG not generated yet; the phone works with GGA+RMC.
                }
            }
        }
    }

    SPDLOG_INFO("[iap2] session ending: identified={} authenticated={} session_started={}",
                identified, authenticated, session_started);
    link.close();

    return !start_abandoned && identified &&
           (authenticated || (options.allow_missing_mfi && !failed));
}

}  // namespace carplay
