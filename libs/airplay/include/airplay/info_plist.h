// SPDX-License-Identifier: GPL-3.0-or-later
//
// The accessory's capability declaration: the plist answering GET /info.
//
// This is the single most consequential message in the session. The phone reads
// it after session SETUP and only proceeds if the declaration is complete --
// display geometry, audio formats and latencies, the CarPlay resource `modes`,
// and HID input devices. A partial or subtly wrong /info does not produce an
// error: the phone accepts the session and then tears it down without ever
// asking for a stream, which is indistinguishable from a transport fault.
//
// It is a pure function of the config, and lives apart from the RTSP server so
// it can be tested that way -- see test_info_plist.cpp.
#ifndef AIRPLAY_INFO_PLIST_H_
#define AIRPLAY_INFO_PLIST_H_

#include "airplay/config.h"
#include "plist/value.h"

namespace airplay
{

plist::Value buildInfoPlist(const ReceiverConfig& config);

}  // namespace airplay

#endif  // AIRPLAY_INFO_PLIST_H_
