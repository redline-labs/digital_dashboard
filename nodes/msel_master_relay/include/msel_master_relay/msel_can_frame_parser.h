#ifndef MSEL_MASTER_RELAY_CAN_FRAME_PARSER_H
#define MSEL_MASTER_RELAY_CAN_FRAME_PARSER_H

#include <array>
#include <cstdint>
#include <functional>

#include "dbc_msel_master_relay.h"

class MselCanFrameParser
{
    using StatusHandler = std::function<void(const dbc_msel_master_relay::Master_Relay_0x6E4_t&)>;
    using InfoHandler = std::function<void(const dbc_msel_master_relay::Master_Relay_0x6E5_t&)>;

  public:
    MselCanFrameParser();

    void handle_can_frame(uint32_t id, const std::array<uint8_t, 8u>& data);

    void on_status(StatusHandler handler);
    void on_info(InfoHandler handler);

  private:
    dbc_msel_master_relay::dbc_msel_master_relay_t db_;
    StatusHandler status_handler_;
    InfoHandler info_handler_;
};

#endif // MSEL_MASTER_RELAY_CAN_FRAME_PARSER_H


