#ifndef MERCEDES_190E_TACHOMETER_CONFIG_H
#define MERCEDES_190E_TACHOMETER_CONFIG_H

#include <string>
#include <cstdint>

struct tachometer_config_t {
    tachometer_config_t() :
        max_rpm{6000},
        redline_rpm{5500},
        show_clock{true}
    {}

    uint16_t max_rpm;           // Maximum RPM value on gauge
    uint16_t redline_rpm;       // RPM where redline zone begins
    bool show_clock;            // Whether to display digital clock
};

#endif // MERCEDES_190E_TACHOMETER_CONFIG_H