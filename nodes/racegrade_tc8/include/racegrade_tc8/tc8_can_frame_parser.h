#ifndef RACEGRADE_TC8_CAN_FRAME_PARSER_H
#define RACEGRADE_TC8_CAN_FRAME_PARSER_H

#include <array>
#include <cstdint>
#include <functional>

#include "dbc_motec_e888_rev1.h"


class Tc8CanFrameParser
{
    using Inputs_message_handler_t = std::function<void(const dbc_motec_e888_rev1::Inputs_t&)>;
    using Diagnostics_message_handler_t = std::function<void(const dbc_motec_e888_rev1::Diagnostics_t&)>;
 
  public:
    Tc8CanFrameParser();

    void handle_can_frame(uint32_t id, const std::array<uint8_t, 8u>& data);

    void set_Input_message_handler(Inputs_message_handler_t handler);
    void set_Diagnostics_message_handler(Diagnostics_message_handler_t handler);

  private:
    dbc_motec_e888_rev1::dbc_motec_e888_rev1_t db_;
    Inputs_message_handler_t Input_message_handler_;
    Diagnostics_message_handler_t Diagnostics_message_handler_;
};


#endif // RACEGRADE_TC8_CAN_FRAME_PARSER_H