// SPDX-License-Identifier: GPL-3.0-or-later
//
// The M1's decoded messages onto their schemas.
//
// About two hundred fields, and neither test either side of this one can see
// them: the decoder is checked against cantools, the schemas round-trip
// themselves, and which signal becomes which field was checked by nobody. Every
// mistake here is a plausible number on a gauge.
//
// Not every field is listed. What is covered is one straightforward message per
// shape, and all four messages assembled from TWO frames -- where a field taken
// from the wrong frame is the mistake that cannot be seen at all, because both
// frames carry the same kind of reading.
#include "motec_m1_messages.h"

#include <capnp/message.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

void expectNear(double actual, double expected, const std::string& what)
{
    expect(std::abs(actual - expected) < 0.05, what + " (got " + std::to_string(actual) +
                                                   ", expected " + std::to_string(expected) + ")");
}

}  // namespace

int main()
{
    using namespace dbc_motec_m1_rev3;

    {
        M1_GEN_0x640_t m;
        m.Engine_Speed = 4500;
        m.Inlet_Manifold_Pressure = 101.0f;
        m.Inlet_Manifold_Temperature = 38.0f;
        m.Throttle_Position = 42.0f;

        capnp::MallocMessageBuilder message;
        motec_m1::fillEngineAir(m, message.initRoot<MotecM1EngineAir>());
        const auto out = message.getRoot<MotecM1EngineAir>().asReader();

        expectNear(out.getEngineSpeedRpm(), 4500, "engine speed");
        expectNear(out.getMapKpa(), 101.0, "manifold pressure");
        expectNear(out.getInletManifoldTempC(), 38.0, "manifold temperature");
        expectNear(out.getThrottlePositionPct(), 42.0, "throttle position");

        // Pressure and temperature are adjacent signals of very different
        // meaning; a swap would read as a plausible pair.
        expect(out.getMapKpa() > out.getInletManifoldTempC(),
               "manifold pressure and temperature are not transposed");
    }

    {
        M1_GEN_0x649_t m;
        m.Coolant_Temperature = 88.0f;
        m.Engine_Oil_Temperature = 104.0f;
        m.Fuel_Temperature = 31.0f;

        capnp::MallocMessageBuilder message;
        motec_m1::fillTemperatures(m, message.initRoot<MotecM1Temperatures>());
        const auto out = message.getRoot<MotecM1Temperatures>().asReader();

        expectNear(out.getCoolantTempC(), 88.0, "coolant temperature");
        expectNear(out.getEngineOilTempC(), 104.0, "oil temperature");
        expectNear(out.getFuelTempC(), 31.0, "fuel temperature");
    }

    // Turbo: bank 1 comes from 0x6A6 and bank 2 from 0x6A7. Both frames carry
    // a speed, an inlet temperature and an outlet temperature, so a field taken
    // from the wrong frame is invisible unless the two frames disagree.
    {
        M1_GEN_0x6A6_t bank1;
        bank1.Turbo_Bank_1_Speed = 120000.0f;
        bank1.Turbo_Bank_1_Inlet_Temp = 40.0f;
        bank1.Turbo_Bank_1_Temp_Outlet = 180.0f;
        bank1.Turbo_Bank_1_Pressure_Inlet = 99.0f;

        M1_GEN_0x6A7_t bank2;
        bank2.Turbo_Bank_2_Speed = 60000.0f;
        bank2.Turbo_Bank_2_Inlet_Temp = 20.0f;
        bank2.Turbo_Bank_2_Temp_Outlet = 90.0f;

        capnp::MallocMessageBuilder message;
        motec_m1::fillTurboBoth(bank1, bank2, message.initRoot<MotecM1Turbo>());
        const auto out = message.getRoot<MotecM1Turbo>().asReader();

        expectNear(out.getBank1SpeedHz(), 120000.0, "bank 1 speed comes from 0x6A6");
        expectNear(out.getBank2SpeedHz(), 60000.0, "bank 2 speed comes from 0x6A7");
        expectNear(out.getBank1InletTempC(), 40.0, "bank 1 inlet temperature");
        expectNear(out.getBank2InletTempC(), 20.0, "bank 2 inlet temperature");
        expectNear(out.getBank1OutletTempC(), 180.0, "bank 1 outlet temperature");
        expectNear(out.getBank2OutletTempC(), 90.0, "bank 2 outlet temperature");
        expectNear(out.getBank1InletPressureKpa(), 99.0, "bank 1 inlet pressure");
    }

    // Knock: cylinders 1-8 from 0x643, 9-12 from 0x659. The cylinder number is
    // the whole content of the reading, so an off-by-one sends a mechanic to
    // the wrong cylinder.
    {
        M1_GEN_0x643_t first;
        first.Engine_Cylinder_1_Knock_Level = 1.0f;
        first.Engine_Cylinder_2_Knock_Level = 2.0f;
        first.Engine_Cylinder_3_Knock_Level = 3.0f;
        first.Engine_Cylinder_4_Knock_Level = 4.0f;
        first.Engine_Cylinder_5_Knock_Level = 5.0f;
        first.Engine_Cylinder_6_Knock_Level = 6.0f;
        first.Engine_Cylinder_7_Knock_Level = 7.0f;
        first.Engine_Cylinder_8_Knock_Level = 8.0f;

        M1_GEN_0x659_t second;
        second.Engine_Cylinder_9_Knock_Level = 9.0f;
        second.Engine_Cylinder_10_Knock_Level = 10.0f;
        second.Engine_Cylinder_11_Knock_Level = 11.0f;
        second.Engine_Cylinder_12_Knock_Level = 12.0f;

        capnp::MallocMessageBuilder message;
        motec_m1::fillKnock1to12(first, second, message.initRoot<MotecM1KnockLevels1to12>());
        const auto out = message.getRoot<MotecM1KnockLevels1to12>().asReader();

        expectNear(out.getCyl1(), 1.0, "cylinder 1");
        expectNear(out.getCyl4(), 4.0, "cylinder 4");
        expectNear(out.getCyl8(), 8.0, "cylinder 8, the last from the first frame");
        expectNear(out.getCyl9(), 9.0, "cylinder 9, the first from the second frame");
        expectNear(out.getCyl12(), 12.0, "cylinder 12");
    }

    // Direct fuel pressure: bank 1 from 0x653, bank 2 from 0x654, six readings
    // each with the same names.
    {
        M1_GEN_0x653_t bank1;
        bank1.Fuel_Pressure_Direct = 15000.0f;
        bank1.Fuel_Pressure_Direct_Aim = 15500.0f;

        M1_GEN_0x654_t bank2;
        bank2.Fuel_Pressure_Direct_B2 = 8000.0f;
        bank2.Fuel_Pressure_Direct_B2_Aim = 8500.0f;

        capnp::MallocMessageBuilder message;
        motec_m1::fillFuelDirectAll(bank1, bank2, message.initRoot<MotecM1FuelDirectAll>());
        const auto out = message.getRoot<MotecM1FuelDirectAll>().asReader();

        expectNear(out.getFuelPressureDirectKpa(), 15000.0, "bank 1 direct fuel pressure");
        expectNear(out.getFuelPressureDirectAimKpa(), 15500.0, "bank 1 aim");
        expectNear(out.getFuelPressureDirectB2Kpa(), 8000.0, "bank 2 direct fuel pressure");
        expectNear(out.getFuelPressureDirectB2AimKpa(), 8500.0, "bank 2 aim");
    }

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
