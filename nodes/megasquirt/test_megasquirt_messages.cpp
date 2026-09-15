// SPDX-License-Identifier: GPL-3.0-or-later
//
// Every field of the Megasquirt dash message, from the decoded frames to the
// schema.
//
// A differential test cannot see this layer, and neither can a screenshot: a
// reading taken from the wrong frame, or two fields transposed, is a plausible
// number on a gauge. Each source field is given a value nothing else has, so a
// swap shows up as the wrong number rather than as a coincidence.
#include "megasquirt_messages.h"

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
    // The decoder's own step, not an arbitrary epsilon: these are float32
    // readings that went through a scale and an offset.
    expect(std::abs(actual - expected) < 0.05, what + " (got " + std::to_string(actual) +
                                                   ", expected " + std::to_string(expected) + ")");
}

}  // namespace

int main()
{
    dbc_megasquirt_dash_data::dbc_megasquirt_dash_data_parser::db_t db;

    db.megasquirt_dash0.rpm = 3100;
    db.megasquirt_dash0.map = 101.5f;
    db.megasquirt_dash0.tps = 42.5f;
    db.megasquirt_dash0.clt = 191.1f;

    db.megasquirt_dash1.adv_deg = 17.3f;
    db.megasquirt_dash1.mat = 104.9f;
    db.megasquirt_dash1.pw1 = 3.21f;
    db.megasquirt_dash1.pw2 = 3.44f;

    db.megasquirt_dash2.pwseq1 = 2.19f;
    db.megasquirt_dash2.egt1 = 1213.0f;
    db.megasquirt_dash2.egocor1 = 101.9f;
    db.megasquirt_dash2.AFR1 = 14.1f;
    db.megasquirt_dash2.afrtgt1 = 14.6f;

    db.megasquirt_dash3.knk_rtd = 1.5f;
    db.megasquirt_dash3.sensors1 = 2.32f;
    db.megasquirt_dash3.sensors2 = 1.33f;
    db.megasquirt_dash3.batt = 13.9f;

    db.megasquirt_dash4.launch_timing = 9.5f;
    db.megasquirt_dash4.tc_retard = 4.25f;
    db.megasquirt_dash4.VSS1 = 27.5f;

    capnp::MallocMessageBuilder message;
    megasquirt::fillDash(db, message.initRoot<MegasquirtDash>());
    const MegasquirtDash::Reader dash = message.getRoot<MegasquirtDash>().asReader();

    expect(dash.getRpm() == 3100, "rpm");
    expectNear(dash.getMapKpa(), 101.5, "manifold pressure");
    expectNear(dash.getTpsPct(), 42.5, "throttle position");
    expectNear(dash.getCoolantTempF(), 191.1, "coolant temperature, in F as the DBC has it");

    expectNear(dash.getIgnitionAdvanceDeg(), 17.3, "ignition advance");
    expectNear(dash.getIntakeAirTempF(), 104.9, "intake air temperature");
    expectNear(dash.getInjPw1Ms(), 3.21, "injector pulse width 1");
    expectNear(dash.getInjPw2Ms(), 3.44, "injector pulse width 2");

    expectNear(dash.getSeqPw1Ms(), 2.19, "sequential pulse width 1");
    expectNear(dash.getEgt1F(), 1213.0, "exhaust gas temperature");
    expectNear(dash.getEgoCorrectionPct(), 101.9, "EGO correction");
    expectNear(dash.getAfr1(), 14.1, "AFR");
    expectNear(dash.getAfrTarget1(), 14.6, "AFR target");

    expectNear(dash.getKnockRetardDeg(), 1.5, "knock retard");
    expectNear(dash.getSensor1(), 2.32, "sensor 1");
    expectNear(dash.getSensor2(), 1.33, "sensor 2");
    expectNear(dash.getBatteryVolts(), 13.9, "battery volts");

    expectNear(dash.getLaunchTimingDeg(), 9.5, "launch timing");
    expectNear(dash.getTcRetard(), 4.25, "traction control retard");
    expectNear(dash.getVssMps(), 27.5, "vehicle speed");

    // The two pairs most easily transposed, stated as their own checks: the
    // frames arrive separately and the fields sit next to each other.
    expect(dash.getInjPw1Ms() < dash.getInjPw2Ms(), "the two injector pulse widths are not swapped");
    expect(dash.getSensor1() > dash.getSensor2(), "the two generic sensors are not swapped");

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
