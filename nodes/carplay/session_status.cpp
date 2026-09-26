// SPDX-License-Identifier: GPL-3.0-or-later
#include "session_status.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace carplay
{

const char* phaseName(SessionPhase phase)
{
    switch (phase)
    {
        case SessionPhase::Idle:             return "idle";
        case SessionPhase::UsbConfig:        return "usb_config";
        case SessionPhase::Lockdown:         return "lockdown";
        case SessionPhase::NcmUp:            return "ncm_up";
        case SessionPhase::Iap2:             return "iap2";
        case SessionPhase::AirplayHandshake: return "airplay_handshake";
        case SessionPhase::Recording:        return "recording";
        case SessionPhase::Error:            return "error";
    }
    return "unknown";
}

SessionStatus::SessionStatus(Publish publish) : publish_(std::move(publish)) {}

void SessionStatus::changePhaseLocked(SessionPhase phase)
{
    SPDLOG_INFO("[node] session phase {} -> {}", phaseName(state_.phase), phaseName(phase));
    if (state_.phase == SessionPhase::Recording)
    {
        state_.mic_active = false;
        state_.mic_sample_rate_hz = 0;
        state_.mic_channels = 0;
    }
    state_.phase = phase;
    state_.device_connected = phase != SessionPhase::Idle;
    if (phase == SessionPhase::Idle)
    {
        state_.device_name.clear();
    }
    publish_(state_);
}

void SessionStatus::setPhase(SessionPhase phase)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase != state_.phase)
    {
        changePhaseLocked(phase);
    }
}

void SessionStatus::setRecording(bool recording)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (recording && state_.phase != SessionPhase::Recording)
    {
        changePhaseLocked(SessionPhase::Recording);
    }
    else if (!recording && state_.phase == SessionPhase::Recording)
    {
        changePhaseLocked(SessionPhase::AirplayHandshake);
    }
}

void SessionStatus::setMicrophone(bool active, uint32_t sample_rate_hz, uint8_t channels)
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_.mic_active = active;
    state_.mic_sample_rate_hz = sample_rate_hz;
    state_.mic_channels = channels;
    publish_(state_);
}

void SessionStatus::setDisplay(bool night_mode, uint16_t width_px, uint16_t height_px)
{
    std::lock_guard<std::mutex> lock(mutex_);
    state_.night_mode = night_mode;
    state_.main_width_px = width_px;
    state_.main_height_px = height_px;
    publish_(state_);
}

void SessionStatus::republish()
{
    std::lock_guard<std::mutex> lock(mutex_);
    publish_(state_);
}

SessionState SessionStatus::state() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

}  // namespace carplay
