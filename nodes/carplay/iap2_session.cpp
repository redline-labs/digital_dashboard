// SPDX-License-Identifier: GPL-3.0-or-later
#include "iap2_session.h"

#include "iap2/link_layer.h"
#include "iap2/mcp2221a_mfi_signer.h"
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

// CarPlayAvailability carries wired_available as a boolean that some phones
// send zero-length. csm::getBool() reports that as absent (LIVI's behaviour),
// which silently suppresses CarPlayStartSession -- so decode it here with
// enough detail to say which case actually occurred.
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
        // Not observed on the phone tested during bring-up, which sends a
        // proper one-byte boolean. Kept as a loud warning because the failure
        // it causes is otherwise silent: availability decodes as absent and
        // CarPlayStartSession is simply never sent. See
        // docs/carplay_bringup.md if this ever fires.
        SPDLOG_WARN("[iap2] CarPlayAvailability wired parameter is ZERO LENGTH -- decoded as "
                    "absent, so CarPlayStartSession will be skipped and the session will "
                    "never start. See docs/carplay_bringup.md.");
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
    std::unique_ptr<iap2::Mcp2221aMfiSigner> owned_signer;
    iap2::MfiSigner* signer = options.signer;
    std::unique_ptr<iap2::MfiAuthenticator> authenticator;
    if (signer != nullptr)
    {
        SPDLOG_INFO("[mfi] using the shared coprocessor, protocol major {}",
                    signer->protocolMajor());
        authenticator = std::make_unique<iap2::MfiAuthenticator>(*signer);
    }
    else
    {
        auto candidate = std::make_unique<iap2::Mcp2221aMfiSigner>();
        if (candidate->init())
        {
            SPDLOG_INFO("[mfi] coprocessor ready, protocol major {}", candidate->protocolMajor());
            owned_signer = std::move(candidate);
            signer = owned_signer.get();
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
    }

    iap2::IdentificationConfig identification;
    bool identified = false;
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
                        // Subscribe to the metadata streams, exactly as LIVI
                        // does once identification and auth are done.
                        SPDLOG_INFO("[iap2] subscribing to now-playing, navigation, call updates");
                        link.sendControlMessage(iap2::encodeStartNowPlayingUpdates());
                        link.sendControlMessage(iap2::encodeStartRouteGuidanceUpdates());
                        link.sendControlMessage(iap2::encodeStartCallStateUpdates());
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

                if (availability->wired_available.value_or(false))
                {
                    SPDLOG_INFO("[iap2] phone reports wired CarPlay AVAILABLE");

                    if (!options.endpoint_provider)
                    {
                        SPDLOG_WARN("[iap2] no accessory endpoint available, so "
                                    "CarPlayStartSession will not be sent (stage 6 not run)");
                        break;
                    }
                    const auto endpoint = options.endpoint_provider();
                    if (!endpoint)
                    {
                        SPDLOG_ERROR("[iap2] the NCM link is not up, so there is no address to "
                                     "hand the phone -- CarPlayStartSession not sent");
                        break;
                    }

                    iap2::CarPlayStartSession session;
                    session.ip_addresses = {endpoint->link_local_address};
                    session.port = endpoint->port;
                    session.device_identifier = endpoint->device_identifier;

                    SPDLOG_INFO("[iap2] sending CarPlayStartSession -> [{}]:{} id={}",
                                endpoint->link_local_address, endpoint->port,
                                endpoint->device_identifier);
                    link.sendControlMessage(iap2::encodeCarPlayStartSession(session));
                    session_started = true;
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
                SPDLOG_INFO("[iap2] now playing: '{}' / '{}' ({})",
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
    auto ft_buffers = std::make_shared<std::map<uint8_t, std::vector<uint8_t>>>();
    link.setFileTransferHandler([&link, &options, ft_buffers](const std::vector<uint8_t>& dgram) {
        if (dgram.size() < 2)
        {
            return;
        }
        constexpr uint8_t kSetup = 0x04;
        constexpr uint8_t kStart = 0x01;
        constexpr uint8_t kFirstData = 0x80;
        constexpr uint8_t kFirstAndOnlyData = 0xC0;
        constexpr uint8_t kData = 0x00;
        constexpr uint8_t kLastData = 0x40;
        constexpr uint8_t kCancel = 0x02;
        constexpr uint8_t kSuccess = 0x05;

        const uint8_t ftid = dgram[0];
        const uint8_t ctrl = dgram[1];
        const std::vector<uint8_t> data(dgram.begin() + 2, dgram.end());

        const auto complete = [&](std::vector<uint8_t> image) {
            ft_buffers->erase(ftid);
            link.sendFileTransfer({ftid, kSuccess});
            SPDLOG_INFO("[iap2] album artwork received: {} bytes", image.size());
            if (options.artwork_handler)
            {
                options.artwork_handler(image);
            }
        };

        switch (ctrl)
        {
            case kSetup:
                (*ft_buffers)[ftid] = {};
                link.sendFileTransfer({ftid, kStart});
                break;
            case kFirstData:
                (*ft_buffers)[ftid] = data;
                break;
            case kData:
                (*ft_buffers)[ftid].insert((*ft_buffers)[ftid].end(), data.begin(), data.end());
                break;
            case kFirstAndOnlyData:
                complete(data);
                break;
            case kLastData:
            {
                auto& buf = (*ft_buffers)[ftid];
                buf.insert(buf.end(), data.begin(), data.end());
                complete(std::move(buf));
                break;
            }
            case kCancel:
                ft_buffers->erase(ftid);
                break;
            default:
                break;
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

    while (!stop.load() && !failed)
    {
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

    return identified && (authenticated || (options.allow_missing_mfi && !failed));
}

}  // namespace carplay
