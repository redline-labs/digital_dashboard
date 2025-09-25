#include "racegrade_tc8/tc8_can_frame_parser.h"

#include <spdlog/spdlog.h>


Tc8CanFrameParser::Tc8CanFrameParser() :
    db_{}
{
}

void Tc8CanFrameParser::handle_can_frame(uint32_t id, const std::array<uint8_t, 8u>& data)
{
    // Enum that contains all the messages in the DBC.
    using Messages = dbc_motec_e888_rev1::dbc_motec_e888_rev1_t::Messages;
    
    auto m = db_.decode(id, data);
    if (m == Messages::Inputs)
    {
        // Once we've seen all defined groups, report and reset
        if (db_.Inputs.all_multiplexed_indexes_seen() == true)
        {
            db_.Inputs.clear_seen_multiplexed_indexes();

            if (Input_message_handler_)
            {
                Input_message_handler_(db_.Inputs);
            }
        }
    }
    else if (m == Messages::Diagnostics)
    {
        if (db_.Diagnostics.all_multiplexed_indexes_seen() == true)
        {
            db_.Diagnostics.clear_seen_multiplexed_indexes();

            if (Diagnostics_message_handler_)
            {
                Diagnostics_message_handler_(db_.Diagnostics);
            }
        }
    }
};

void Tc8CanFrameParser::set_Input_message_handler(Inputs_message_handler_t handler)
{
    Input_message_handler_ = handler;
}

void Tc8CanFrameParser::set_Diagnostics_message_handler(Diagnostics_message_handler_t handler)
{
    Diagnostics_message_handler_ = handler;
}