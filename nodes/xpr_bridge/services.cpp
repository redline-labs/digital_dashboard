// SPDX-License-Identifier: GPL-3.0-or-later

#include "services.h"

#include <optional>
#include <string>

#include <spdlog/spdlog.h>

#include "xpr_radio.capnp.h"
#include "pub_sub/zenoh_service.h"

namespace xpr_node
{

xpr::Result<ChannelState> read_channel(xpr::Radio& radio)
{
    const xpr::Result<mototrbo::control::ZoneChannel> where = radio.channel();
    if (!where.has_value())
    {
        return std::unexpected(where.error());
    }

    ChannelState state;
    state.zone = where->zone;
    state.channel = where->channel;

    // The counts are a separate pair of queries, and they are allowed to fail
    // without failing the read: knowing the radio is on channel 3 is worth
    // more than knowing nothing because it would not say how many there are.
    if (const xpr::Result<xpr::Radio::ChannelCounts> counts = radio.channelCounts(); counts.has_value())
    {
        state.zoneCount = counts->zones;
        state.channelsInZone = counts->channelsInZone;
    }

    return state;
}

namespace
{

// Carry the counts forward from what we already knew, so a refreshed channel
// that could not read the counts does not report zero of zero.
ChannelState withKnownCounts(ChannelState fresh, const std::optional<ChannelState>& previous)
{
    if (previous.has_value())
    {
        if (fresh.zoneCount == 0)
        {
            fresh.zoneCount = previous->zoneCount;
        }
        if (fresh.channelsInZone == 0)
        {
            fresh.channelsInZone = previous->channelsInZone;
        }
    }

    return fresh;
}

} // namespace

struct Services::Impl
{
    Deps deps;

    std::optional<pub_sub::ZenohService<::XprGetChannelRequest, ::XprGetChannelResponse>> getChannel;
    std::optional<pub_sub::ZenohService<::XprSetChannelRequest, ::XprSetChannelResponse>> setChannel;
    std::optional<pub_sub::ZenohService<::XprGetIdentityRequest, ::XprGetIdentityResponse>> getIdentity;
};

Services::Services(Deps deps) : mImpl(std::make_unique<Impl>())
{
    mImpl->deps = deps;

    const std::string prefix = deps.config->publish.topicPrefix;

    mImpl->getChannel.emplace(
        prefix + "/get_channel",
        [impl = mImpl.get()](const ::XprGetChannelRequest::Reader& request,
                             ::XprGetChannelResponse::Builder& response) {
            std::optional<ChannelState> state = impl->deps.state->channel();

            if (request.getRefresh() || !state.has_value())
            {
                const xpr::Result<ChannelState> fresh = read_channel(*impl->deps.radio);
                if (!fresh.has_value())
                {
                    response.setOk(false);
                    response.setError(xpr::to_string(fresh.error()));
                    return;
                }

                state = withKnownCounts(*fresh, state);
                impl->deps.state->setChannel(*state);
            }

            response.setOk(true);
            fillChannel(response.initChannel(), *state);
        });

    mImpl->setChannel.emplace(
        prefix + "/set_channel",
        [impl = mImpl.get()](const ::XprSetChannelRequest::Reader& request,
                             ::XprSetChannelResponse::Builder& response) {
            // Refused here rather than at the radio. This moves somebody's
            // radio off the channel they are listening to, so it is opt-in
            // per deployment and the refusal says which setting to change.
            if (!impl->deps.config->control.allowChannelChange)
            {
                response.setOk(false);
                response.setError("channel control is disabled (set control.allow_channel_change)");
                return;
            }

            xpr::Result<mototrbo::control::ZoneChannel> moved =
                std::unexpected(xpr::Error { xpr::Error::Kind::InvalidArgument, "no operation given", 0 });

            switch (request.getOp())
            {
                case ::XprChannelOp::UP:
                    moved = impl->deps.radio->stepChannel(true);
                    break;
                case ::XprChannelOp::DOWN:
                    moved = impl->deps.radio->stepChannel(false);
                    break;
                case ::XprChannelOp::SELECT:
                    moved = impl->deps.radio->selectChannel(request.getZone(), request.getChannel());
                    break;
                case ::XprChannelOp::UNKNOWN:
                    break;
            }

            // Where the radio ENDED UP, even when the move failed part way:
            // a select that stepped twice and then stopped has left the radio
            // somewhere, and a caller that is told only "failed" would have to
            // guess where.
            const std::optional<ChannelState> previous = impl->deps.state->channel();
            ChannelState state;
            if (moved.has_value())
            {
                state.zone = moved->zone;
                state.channel = moved->channel;
            }
            else if (const xpr::Result<ChannelState> after = read_channel(*impl->deps.radio);
                     after.has_value())
            {
                state = *after;
            }
            else if (previous.has_value())
            {
                state = *previous;
            }

            state = withKnownCounts(state, previous);
            impl->deps.state->setChannel(state);

            response.setOk(moved.has_value());
            if (!moved.has_value())
            {
                response.setError(xpr::to_string(moved.error()));
            }
            fillChannel(response.initChannel(), state);
        });

    mImpl->getIdentity.emplace(
        prefix + "/get_identity",
        [impl = mImpl.get()](const ::XprGetIdentityRequest::Reader& request,
                             ::XprGetIdentityResponse::Builder& response) {
            std::optional<xpr::Radio::Identity> identity = impl->deps.state->identity();

            if (request.getRefresh() || !identity.has_value())
            {
                const xpr::Result<xpr::Radio::Identity> fresh = impl->deps.radio->identity();
                if (!fresh.has_value())
                {
                    response.setOk(false);
                    response.setError(xpr::to_string(fresh.error()));
                    return;
                }

                identity = *fresh;
                impl->deps.state->setIdentity(*identity);
            }

            response.setOk(true);
            fillIdentity(response.initIdentity(), *identity);
        });

    SPDLOG_INFO("xpr: services on {}/get_channel, {}/set_channel, {}/get_identity", prefix, prefix, prefix);
}

Services::~Services() = default;

} // namespace xpr_node
