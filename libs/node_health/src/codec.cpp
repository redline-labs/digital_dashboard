// SPDX-License-Identifier: GPL-3.0-or-later
#include "node_health/codec.h"

#include "pub_sub/capnp_payload.h"

#include <capnp/serialize.h>
#include <kj/exception.h>

#include <algorithm>
#include <string>

namespace node_health
{

namespace
{

::HealthState toWire(State state)
{
    switch (state)
    {
        case State::unknown:
            return ::HealthState::UNKNOWN;
        case State::starting:
            return ::HealthState::STARTING;
        case State::ok:
            return ::HealthState::OK;
        case State::degraded:
            return ::HealthState::DEGRADED;
        case State::fault:
            return ::HealthState::FAULT;
        case State::stopping:
            return ::HealthState::STOPPING;
    }
    return ::HealthState::UNKNOWN;
}

State fromWire(::HealthState state)
{
    switch (state)
    {
        case ::HealthState::UNKNOWN:
            return State::unknown;
        case ::HealthState::STARTING:
            return State::starting;
        case ::HealthState::OK:
            return State::ok;
        case ::HealthState::DEGRADED:
            return State::degraded;
        case ::HealthState::FAULT:
            return State::fault;
        case ::HealthState::STOPPING:
            return State::stopping;
    }
    // A newer node's enumerant. After the switch rather than in a default:, so
    // a value added to the schema is still a compile error above.
    return State::unknown;
}

// Truncated on a UTF-8 boundary, so a cut never leaves half a character.
std::string bounded(::capnp::Text::Reader text)
{
    std::string value(text.cStr(), text.size());
    if (value.size() <= kMaxTextBytes)
    {
        return value;
    }
    std::size_t cut = kMaxTextBytes;
    while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xc0u) == 0x80u)
    {
        --cut;
    }
    value.resize(cut);
    return value;
}

}  // namespace

void encode(::NodeHealth::Builder out, const HealthSnapshot& snapshot)
{
    out.setNode(snapshot.node);
    out.setZid(snapshot.zid);
    out.setState(toWire(snapshot.state));
    out.setSequence(snapshot.sequence);
    out.setUptimeMs(snapshot.uptime_ms);
    out.setPeriodMs(snapshot.period_ms);
    out.setPid(snapshot.pid);

    const std::size_t count = std::min(snapshot.checks.size(), kMaxChecks);
    auto checks = out.initChecks(static_cast<unsigned>(count));
    for (unsigned i = 0u; i < count; ++i)
    {
        const CheckReport& report = snapshot.checks[i];
        auto check = checks[i];
        check.setName(report.name);
        check.setState(toWire(report.state));
        check.setDetail(report.detail);
        check.setStateAgeMs(report.state_age_ms);
    }
}

HealthSnapshot decode(::NodeHealth::Reader in)
{
    HealthSnapshot snapshot;
    snapshot.node = bounded(in.getNode());
    snapshot.zid = bounded(in.getZid());
    snapshot.state = fromWire(in.getState());
    snapshot.sequence = in.getSequence();
    snapshot.uptime_ms = in.getUptimeMs();
    snapshot.period_ms = in.getPeriodMs();
    snapshot.pid = in.getPid();

    const auto checks = in.getChecks();
    const std::size_t count = std::min<std::size_t>(checks.size(), kMaxChecks);
    snapshot.checks.reserve(count);
    for (const auto check : checks)
    {
        if (snapshot.checks.size() == count)
        {
            break;
        }
        snapshot.checks.push_back(CheckReport{
            .name = bounded(check.getName()),
            .state = fromWire(check.getState()),
            .detail = bounded(check.getDetail()),
            .state_age_ms = check.getStateAgeMs(),
        });
    }
    return snapshot;
}

std::optional<HealthSnapshot> decodePayload(const std::vector<std::uint8_t>& payload)
{
    const pub_sub::WordAlignedPayload aligned(payload);
    if (aligned.words().size() == 0)
    {
        return std::nullopt;
    }
    try
    {
        ::capnp::FlatArrayMessageReader reader(aligned.words());
        return decode(reader.getRoot<::NodeHealth>());
    }
    catch (const kj::Exception&)
    {
        return std::nullopt;
    }
}

}  // namespace node_health
