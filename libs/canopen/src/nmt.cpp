// SPDX-License-Identifier: GPL-3.0-or-later

#include "canopen/nmt.h"

#include <spdlog/fmt/fmt.h>

namespace canopen
{

const char* to_string(NmtCommand command)
{
    switch (command)
    {
    case NmtCommand::Start: return "start (operational)";
    case NmtCommand::Stop: return "stop";
    case NmtCommand::EnterPreOperational: return "enter pre-operational";
    case NmtCommand::ResetNode: return "reset node";
    case NmtCommand::ResetCommunication: return "reset communication";
    }
    return "unknown";
}

const char* to_string(NmtState state)
{
    switch (state)
    {
    case NmtState::BootUp: return "boot-up";
    case NmtState::Stopped: return "stopped";
    case NmtState::Operational: return "operational";
    case NmtState::PreOperational: return "pre-operational";
    }
    return "unknown";
}

std::string to_string(const EmcyMessage& message)
{
    return fmt::format("node {} emergency 0x{:04X}, error register 0x{:02X}, "
                       "manufacturer {:02X} {:02X} {:02X} {:02X} {:02X}",
                       message.nodeId, message.errorCode, message.errorRegister,
                       message.manufacturerSpecific[0], message.manufacturerSpecific[1],
                       message.manufacturerSpecific[2], message.manufacturerSpecific[3],
                       message.manufacturerSpecific[4]);
}

NmtMaster::NmtMaster(Bus& bus)
    : bus_(bus)
{
    bus_.subscribe(
        [this](const helpers::CanFrame& frame)
        {
            const uint32_t id = frame.id & 0x7FF;

            if (id >= kHeartbeatCobIdBase && id < kHeartbeatCobIdBase + 128 && frame.len >= 1)
            {
                const uint8_t nodeId = static_cast<uint8_t>(id - kHeartbeatCobIdBase);
                // The toggle bit in bit 7 alternates on every heartbeat from
                // some devices; the state is the low seven bits.
                const auto state = static_cast<NmtState>(frame.data[0] & 0x7F);

                if (frame.data[0] == 0x00)
                {
                    ++bootCounts_[nodeId];
                }

                const auto previous = states_.find(nodeId);
                const bool changed = previous == states_.end() || previous->second != state;
                states_[nodeId] = state;
                if (changed && stateCallback_)
                {
                    stateCallback_(nodeId, state);
                }
                return;
            }

            if (id > kEmcyCobIdBase && id < kEmcyCobIdBase + 128 && frame.len >= 8)
            {
                EmcyMessage message;
                message.nodeId = static_cast<uint8_t>(id - kEmcyCobIdBase);
                message.errorCode = static_cast<uint16_t>(frame.data[0] | (frame.data[1] << 8));
                message.errorRegister = frame.data[2];
                for (size_t i = 0; i < message.manufacturerSpecific.size(); ++i)
                {
                    message.manufacturerSpecific[i] = frame.data[3 + i];
                }
                if (emergencyCallback_)
                {
                    emergencyCallback_(message);
                }
            }
        });
}

helpers::CanFrame make_nmt_frame(NmtCommand command, uint8_t nodeId)
{
    helpers::CanFrame frame {};
    frame.id = NmtMaster::kNmtCobId;
    frame.len = 2;
    frame.data[0] = static_cast<uint8_t>(command);
    frame.data[1] = nodeId;
    return frame;
}

void NmtMaster::command(NmtCommand command, uint8_t nodeId)
{
    bus_.send(make_nmt_frame(command, nodeId));
}

std::optional<NmtState> NmtMaster::state(uint8_t nodeId) const
{
    auto it = states_.find(nodeId);
    if (it == states_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

bool NmtMaster::reset_and_wait(uint8_t nodeId, Duration timeout)
{
    const uint32_t before = bootCounts_[nodeId];
    command(NmtCommand::ResetNode, nodeId);
    return wait_until(bus_, timeout, [&] { return bootCounts_[nodeId] > before; });
}

bool NmtMaster::wait_for_bootup(uint8_t nodeId, Duration timeout)
{
    const uint32_t before = bootCounts_[nodeId];
    return wait_until(bus_, timeout, [&] { return bootCounts_[nodeId] > before; });
}

void NmtMaster::on_emergency(std::function<void(const EmcyMessage&)> callback)
{
    emergencyCallback_ = std::move(callback);
}

void NmtMaster::on_state_change(std::function<void(uint8_t, NmtState)> callback)
{
    stateCallback_ = std::move(callback);
}

} // namespace canopen
