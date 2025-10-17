#include "canopen/transport.h"

namespace canopen
{

// Build NMT command frame (ID 0x000)
helpers::CanFrame make_nmt(NmtCommand cmd, uint8_t nodeId)
{
    helpers::CanFrame f{};
    f.id = 0x000;
    f.len = 2;
    f.data[0] = static_cast<uint8_t>(cmd);
    f.data[1] = nodeId;
    return f;
}

// SDO expedited download to 16-bit value (index/subindex) to node
helpers::CanFrame make_sdo_download_u16(uint8_t nodeId, uint16_t index, uint8_t subindex, uint16_t value)
{
    helpers::CanFrame f{};
    f.id = 0x600 + nodeId; // client->server
    f.len = 8;
    f.data[0] = 0x2B; // expedited, size 2 bytes
    f.data[1] = static_cast<uint8_t>(index & 0xFFu);
    f.data[2] = static_cast<uint8_t>((index >> 8) & 0xFFu);
    f.data[3] = subindex;
    f.data[4] = static_cast<uint8_t>(value & 0xFFu);
    f.data[5] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    return f;
}

} // namespace canopen
