// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/stack/nalu.ts
#ifndef AIRPLAY_NALU_H_
#define AIRPLAY_NALU_H_

#include <cstdint>
#include <optional>
#include <vector>

namespace airplay::nalu
{

using Bytes = std::vector<uint8_t>;

enum class Codec
{
    H264,
    H265,
};

// CarPlay screen frames carry NAL units each prefixed with a big-endian length
// (avcC/hvcC style) and the codec config arrives as an avcC/hvcC atom; decoders
// downstream want Annex-B (00 00 00 01 start codes). Only the framing is
// rewritten here, never the payload.

// Rewrites a run of length-prefixed NAL units to Annex-B. length_size is 1..4.
Bytes avccFrameToAnnexB(const Bytes& frame, size_t length_size = 4);

struct Config
{
    Codec codec = Codec::H264;
    Bytes annex_b;  // the parameter sets (VPS/SPS/PPS) as an Annex-B stream
};

// Converts a VideoConfig payload into Annex-B parameter sets and detects the
// codec. The payload is either a bare avcC/hvcC record, a box
// ([size]['avcC'|'hvcC'][record]), or an 'avc1'/'hvc1' sample entry with the
// config box nested inside (H.265 arrives this way). Returns nullopt when no
// parameter set could be extracted.
std::optional<Config> configToAnnexB(const Bytes& payload);

// Keyframe detection for the node layer's isKeyframe flag: IDR (type 5) for
// H.264, IRAP (types 16..23) for H.265.
bool avccContainsKeyframe(const Bytes& frame, Codec codec, size_t length_size = 4);
bool annexBContainsKeyframe(const Bytes& stream, Codec codec);

// True when this single NAL unit header byte (or the first byte of a two-byte
// H.265 header) marks a keyframe slice.
bool isKeyframeNalu(uint8_t header_byte, Codec codec);

}  // namespace airplay::nalu

#endif  // AIRPLAY_NALU_H_
