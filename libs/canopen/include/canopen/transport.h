#ifndef CANOPEN_TRANSPORT_H
#define CANOPEN_TRANSPORT_H

#include <cstdint>
#include "helpers/can_frame.h"

namespace canopen
{

// NMT commands
enum class NmtCommand : uint8_t
{
    Start = 0x01,
    Stop = 0x02,
    EnterPreOperational = 0x80,
    ResetNode = 0x81,
    ResetCommunication = 0x82,
};

helpers::CanFrame make_nmt(NmtCommand cmd, uint8_t nodeId);
helpers::CanFrame make_sdo_download_u16(uint8_t nodeId, uint16_t index, uint8_t subindex, uint16_t value);

} // namespace canopen

#endif // CANOPEN_TRANSPORT_H


