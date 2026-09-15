// SPDX-License-Identifier: GPL-3.0-or-later
#include "megasquirt_messages.h"

namespace megasquirt
{

void fillDash(const dbc_megasquirt_dash_data::dbc_megasquirt_dash_data_parser::db_t& db, MegasquirtDash::Builder out)
{
    const auto& dash0 = db.megasquirt_dash0;
    const auto& dash1 = db.megasquirt_dash1;
    const auto& dash2 = db.megasquirt_dash2;
    const auto& dash3 = db.megasquirt_dash3;
    const auto& dash4 = db.megasquirt_dash4;

    // Frame 0
    out.setRpm(dash0.rpm);
    out.setMapKpa(dash0.map);
    out.setTpsPct(dash0.tps);
    out.setCoolantTempF(dash0.clt);

    // Frame 1
    out.setIgnitionAdvanceDeg(dash1.adv_deg);
    out.setIntakeAirTempF(dash1.mat);
    out.setInjPw1Ms(dash1.pw1);
    out.setInjPw2Ms(dash1.pw2);

    // Frame 2
    out.setSeqPw1Ms(dash2.pwseq1);
    out.setEgt1F(dash2.egt1);
    out.setEgoCorrectionPct(dash2.egocor1);
    out.setAfr1(dash2.AFR1);
    out.setAfrTarget1(dash2.afrtgt1);

    // Frame 3
    out.setKnockRetardDeg(dash3.knk_rtd);
    out.setSensor1(dash3.sensors1);
    out.setSensor2(dash3.sensors2);
    out.setBatteryVolts(dash3.batt);

    // Frame 4
    out.setLaunchTimingDeg(dash4.launch_timing);
    out.setTcRetard(dash4.tc_retard);
    out.setVssMps(dash4.VSS1);

}

}  // namespace megasquirt
