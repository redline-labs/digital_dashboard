#include "msel_master_relay/msel_can_frame_parser.h"

#include <spdlog/spdlog.h>

MselCanFrameParser::MselCanFrameParser() :
    db_{}
{
}

void MselCanFrameParser::handle_can_frame(uint32_t id, const std::array<uint8_t, 8u>& data)
{
    // Enum that contains all the messages in the DBC.
    using Messages = dbc_msel_master_relay::dbc_msel_master_relay_t::Messages;

    auto m = db_.decode(id, data);
    if (m == Messages::Master_Relay_0x6E4)
    {
        if (status_handler_)
        {
            status_handler_(db_.Master_Relay_0x6E4);
        }
    }
    else if (m == Messages::Master_Relay_0x6E5)
    {
        if (info_handler_)
        {
            info_handler_(db_.Master_Relay_0x6E5);
        }
    }
}

void MselCanFrameParser::on_status(StatusHandler handler)
{
    status_handler_ = handler;
}

void MselCanFrameParser::on_info(InfoHandler handler)
{
    info_handler_ = handler;
}


