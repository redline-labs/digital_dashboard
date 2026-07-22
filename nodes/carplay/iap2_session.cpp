// SPDX-License-Identifier: GPL-3.0-or-later
#include "iap2_session.h"

#include "iap2/link_layer.h"
#include "iap2/mcp2221a_mfi_signer.h"
#include "iap2/messages.h"

#include <spdlog/spdlog.h>
#include <spdlog/fmt/ranges.h>

#include <chrono>
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
        SPDLOG_WARN("[iap2] CarPlayAvailability wired parameter is ZERO LENGTH -- decoded as "
                    "absent, so CarPlayStartSession would be skipped. This is the case "
                    "docs/carplay_bringup.md flags; re-run with --iap2-zero-bool-true to "
                    "treat it as present.");
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
    std::unique_ptr<iap2::Mcp2221aMfiSigner> signer;
    std::unique_ptr<iap2::MfiAuthenticator> authenticator;
    {
        auto candidate = std::make_unique<iap2::Mcp2221aMfiSigner>();
        if (candidate->init())
        {
            SPDLOG_INFO("[mfi] coprocessor ready, protocol major {}", candidate->protocolMajor());
            signer = std::move(candidate);
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
    bool failed = false;

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

                const bool wired_ok =
                    availability->wired_available.value_or(options.zero_length_bool_is_true);
                if (wired_ok)
                {
                    SPDLOG_INFO("[iap2] phone reports wired CarPlay AVAILABLE. "
                                "CarPlayStartSession needs the NCM link-local address "
                                "(stage 6), which is not wired up yet.");
                }
                else
                {
                    SPDLOG_WARN("[iap2] wired CarPlay reported unavailable -- not starting a "
                                "session.");
                }
                break;
            }

            default:
                SPDLOG_DEBUG("[iap2] unhandled {} (0x{:04x})",
                             iap2::messageIdName(message->id), message->id);
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
    }

    SPDLOG_INFO("[iap2] session ending: identified={} authenticated={}", identified, authenticated);
    link.close();

    return identified && (authenticated || (options.allow_missing_mfi && !failed));
}

}  // namespace carplay
