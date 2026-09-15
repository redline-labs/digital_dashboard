// SPDX-License-Identifier: GPL-3.0-or-later
#include "motec_ltc_messages.h"

namespace motec_ltc
{

void fillTelemetry(const dbc_motec_ltc_rev1::LTC_1_ID1_t& m, MotecLtcTelemetry::Builder out)
{
    using SensorState = dbc_motec_ltc_rev1::LTC_1_ID1_t::sig_LTC1_SensorState_t::Values;

    out.setIndex(m.LTC1_Index);
    out.setLambda(m.LTC1_Lambda);
    out.setIpn(m.LTC1_Ipn);
    out.setInternalTempC(m.LTC1_InternalTemp);

    out.setSensorControlFault(static_cast<bool>(m.LTC1_SensorControlFault));
    out.setInternalFault(static_cast<bool>(m.LTC1_InternalFault));
    out.setSensorWireShort(static_cast<bool>(m.LTC1_SensorWireShort));
    out.setHeaterFailedToHeat(static_cast<bool>(m.LTC1_HeaterFailedtoHeat));
    out.setHeaterOpenCircuit(static_cast<bool>(m.LTC1_HeaterOpenCircuit));
    out.setHeaterShortToVbatt(static_cast<bool>(m.LTC1_HeaterShorttoVBATT));
    out.setHeaterShortToGnd(static_cast<bool>(m.LTC1_HeaterShorttoGND));

    out.setHeaterDutyCyclePct(m.LTC1_HeaterDutyCycle);

    switch (m.LTC1_SensorState)
    {
        case SensorState::START:
            out.setSensorState(LtcSensorState::START);
            break;

        case SensorState::DIAGNOSTICS:
            out.setSensorState(LtcSensorState::DIAGNOSTICS);
            break;

        case SensorState::PRE_CAL:
            out.setSensorState(LtcSensorState::PRE_CAL);
            break;

        case SensorState::CALIBRATION:
            out.setSensorState(LtcSensorState::CALIBRATION);
            break;

        case SensorState::POST_CAL:
            out.setSensorState(LtcSensorState::POST_CAL);
            break;

        case SensorState::PAUSED:
            out.setSensorState(LtcSensorState::PAUSED);
            break;

        case SensorState::HEATING:
            out.setSensorState(LtcSensorState::HEATING);
            break;

        case SensorState::RUNNING:
            out.setSensorState(LtcSensorState::RUNNING);
            break;

        case SensorState::COOLING:
            out.setSensorState(LtcSensorState::COOLING);
            break;

        case SensorState::PUMP_START:
            out.setSensorState(LtcSensorState::PUMP_START);
            break;

        case SensorState::PUMP_OFF:
            out.setSensorState(LtcSensorState::PUMP_OFF);
            break;

        default:
            out.setSensorState(LtcSensorState::START);
            break;
    }

    out.setBattVolts(m.LTC1_BattVolts);
    out.setIp(m.LTC1_Ip);
    out.setRi(m.LTC1_Ri);

}

}  // namespace motec_ltc
