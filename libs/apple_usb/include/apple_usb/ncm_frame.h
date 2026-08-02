// SPDX-License-Identifier: GPL-3.0-or-later
//
// CDC-NCM transfer-block framing, and the EUI-64 link-local derivation that
// goes with it. Extracted from ncm_bridge.cpp for the same reason
// ncm_discovery.h was: these are pure functions of their arguments, so pulling
// them out of the Linux-only bridge makes them compile and unit test on every
// platform. They are the wire format on the CarPlay AV data path -- every video
// frame and audio packet the phone sends crosses parseNtb() -- and while they
// lived as private members of NcmBridge no test could reach them at all.
//
// Nothing here touches USB, a socket or the network: hand it bytes, get bytes.
#ifndef APPLE_USB_NCM_FRAME_H_
#define APPLE_USB_NCM_FRAME_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace apple_usb
{

// NTB16 signatures, little-endian as they appear on the wire.
inline constexpr uint32_t kNth16Signature = 0x484D434E;  // "NCMH"
inline constexpr uint32_t kNdp16Signature = 0x304D434E;  // "NCM0"
// The last signature byte is '0' (no CRC) or '1' (CRC); accept either.
inline constexpr uint32_t kNdp16SignatureMask = 0x00FFFFFF;

inline constexpr size_t kNth16Length = 12;  // wHeaderLength we emit
inline constexpr size_t kNdp16Length = 16;  // NDP16 header + 1 entry + terminator
inline constexpr size_t kTxDatagramOffset = kNth16Length + kNdp16Length;  // 28

// ---------------- NTB16 framing ----------------
//
// An NTB16 (NCM Transfer Block, 16-bit variant) is:
//
//   NTH16 @0        dwSignature "NCMH", wHeaderLength, wSequence,
//                   wBlockLength, wNdpIndex                        (12 bytes)
//   NDP16 @wNdpIndex
//                   dwSignature "NCM0"/"NCM1", wLength,
//                   wNextNdpIndex, then a datagram pointer table of
//                   (wDatagramIndex, wDatagramLength) pairs
//                   terminated by a (0, 0) entry
//   datagrams       raw ethernet frames at the offsets the table names
//
// All fields are little-endian and all offsets are from the start of the
// block.

// One NTB16 block -> the ethernet datagrams it carries. Malformed blocks yield
// whatever could be recovered and a warning naming the offending offset; this
// never throws and never reads outside `ntb`.
std::vector<std::vector<uint8_t>> parseNtb(const std::vector<uint8_t>& ntb);

// One ethernet frame -> a single-datagram NTB16 block.
//
// `seq` is the caller's rolling wSequence and is pre-incremented, so the caller
// owns the counter (and whatever lock guards it) rather than this function
// holding state. `out_max` is the device's dwNtbOutMaxSize, used only to warn
// when the block would exceed what the device advertised.
std::vector<uint8_t> buildNtb(const uint8_t* frame, size_t len, uint16_t& seq, uint32_t out_max);

// EUI-64 IPv6 link-local from a MAC, exactly as LIVI's
// cp_handler._iface_eui64_fe80: flip the universal/local bit of the first
// octet and insert ff:fe in the middle. Returns "" if `mac` is not exactly six
// colon-separated hex octets.
std::string deriveEui64LinkLocal(const std::string& mac);

}  // namespace apple_usb

#endif  // APPLE_USB_NCM_FRAME_H_
