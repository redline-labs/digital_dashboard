// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/stack/nalu.ts
#include "airplay/nalu.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace airplay::nalu
{

namespace
{

constexpr uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

constexpr uint8_t kH264NaluIdr = 5;
constexpr uint8_t kH265NaluIrapLow = 16;   // BLA_W_LP
constexpr uint8_t kH265NaluIrapHigh = 23;  // RSV_IRAP_VCL23

void appendStartCode(Bytes& out)
{
    out.insert(out.end(), std::begin(kStartCode), std::end(kStartCode));
}

uint16_t readU16(const Bytes& b, size_t offset)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(b[offset]) << 8) | b[offset + 1]);
}

bool matchesFourCc(const Bytes& b, size_t offset, const char (&cc)[5])
{
    return offset + 4 <= b.size() && std::memcmp(b.data() + offset, cc, 4) == 0;
}

// avcC record: [1][profile][compat][level][fc|lengthSizeMinusOne][e0|numSPS]...
Bytes avcCToAnnexB(const Bytes& atom, size_t start)
{
    Bytes out;
    size_t p = start + 5;  // version, profile, compat, level, lengthSizeMinusOne
    if (p >= atom.size())
    {
        SPDLOG_ERROR("[airplay] nalu: avcC atom too short ({} bytes)", atom.size() - std::min(start, atom.size()));
        return out;
    }

    const uint8_t num_sps = atom[p++] & 0x1f;
    for (uint8_t i = 0; i < num_sps && p + 2 <= atom.size(); ++i)
    {
        const size_t length = readU16(atom, p);
        p += 2;
        if (p + length > atom.size())
        {
            SPDLOG_ERROR("[airplay] nalu: avcC SPS {} runs past the atom ({} of {} bytes)", i, length,
                         atom.size() - p);
            return out;
        }
        appendStartCode(out);
        out.insert(out.end(), atom.begin() + static_cast<ptrdiff_t>(p),
                   atom.begin() + static_cast<ptrdiff_t>(p + length));
        p += length;
    }

    if (p >= atom.size())
    {
        return out;
    }
    const uint8_t num_pps = atom[p++];
    for (uint8_t i = 0; i < num_pps && p + 2 <= atom.size(); ++i)
    {
        const size_t length = readU16(atom, p);
        p += 2;
        if (p + length > atom.size())
        {
            SPDLOG_ERROR("[airplay] nalu: avcC PPS {} runs past the atom ({} of {} bytes)", i, length,
                         atom.size() - p);
            return out;
        }
        appendStartCode(out);
        out.insert(out.end(), atom.begin() + static_cast<ptrdiff_t>(p),
                   atom.begin() + static_cast<ptrdiff_t>(p + length));
        p += length;
    }
    return out;
}

// hvcC record: a fixed 22-byte profile/level block, then arrays of NAL units.
Bytes hvcCToAnnexB(const Bytes& atom, size_t start)
{
    Bytes out;
    size_t p = start + 22;
    if (p >= atom.size())
    {
        SPDLOG_ERROR("[airplay] nalu: hvcC atom too short ({} bytes)", atom.size() - std::min(start, atom.size()));
        return out;
    }

    const uint8_t num_arrays = atom[p++];
    for (uint8_t a = 0; a < num_arrays && p + 3 <= atom.size(); ++a)
    {
        ++p;  // array_completeness + reserved + NAL_unit_type
        const size_t num_nalus = readU16(atom, p);
        p += 2;
        for (size_t i = 0; i < num_nalus && p + 2 <= atom.size(); ++i)
        {
            const size_t length = readU16(atom, p);
            p += 2;
            if (p + length > atom.size())
            {
                SPDLOG_ERROR("[airplay] nalu: hvcC NALU {}/{} runs past the atom ({} of {} bytes)", a, i, length,
                             atom.size() - p);
                return out;
            }
            appendStartCode(out);
            out.insert(out.end(), atom.begin() + static_cast<ptrdiff_t>(p),
                       atom.begin() + static_cast<ptrdiff_t>(p + length));
            p += length;
        }
    }
    return out;
}

// A bare avcC starts [1][profile][compat][level][fc|len][e0|numSPS][spsLen][SPS…]
// and the first SPS NAL unit type is 7.
bool looksLikeAvcC(const Bytes& atom)
{
    if (atom.size() < 9)
    {
        return false;
    }
    if ((atom[5] & 0x1f) < 1)
    {
        return false;  // numOfSPS
    }
    const size_t sps_length = readU16(atom, 6);
    if (8 + sps_length > atom.size())
    {
        return false;
    }
    return (atom[8] & 0x1f) == 7;
}

// Walks the length-prefixed NAL units in a frame, calling `visit(ptr, length)`.
// Stops on the first malformed length, exactly like the TypeScript original.
template <typename Fn>
void forEachAvccNalu(const Bytes& frame, size_t length_size, Fn&& visit)
{
    size_t offset = 0;
    while (offset + length_size <= frame.size())
    {
        size_t length = 0;
        for (size_t i = 0; i < length_size; ++i)
        {
            length = (length << 8) | frame[offset + i];
        }
        offset += length_size;
        if (length == 0 || offset + length > frame.size())
        {
            break;
        }
        visit(offset, length);
        offset += length;
    }
}

}  // namespace

Bytes avccFrameToAnnexB(const Bytes& frame, size_t length_size)
{
    Bytes out;
    if (length_size < 1 || length_size > 4)
    {
        SPDLOG_ERROR("[airplay] nalu: bad length prefix size {} (frame {} bytes)", length_size, frame.size());
        return out;
    }

    out.reserve(frame.size() + 8);
    forEachAvccNalu(frame, length_size,
                    [&](size_t offset, size_t length)
                    {
                        appendStartCode(out);
                        out.insert(out.end(), frame.begin() + static_cast<ptrdiff_t>(offset),
                                   frame.begin() + static_cast<ptrdiff_t>(offset + length));
                    });
    return out;
}

std::optional<Config> configToAnnexB(const Bytes& payload)
{
    // The config record starts right after the avcC/hvcC fourcc, wherever it
    // sits: bare box, or nested inside an avc1/hvc1 sample entry.
    for (size_t i = 4; i + 4 <= payload.size(); ++i)
    {
        if (matchesFourCc(payload, i, "hvcC"))
        {
            Config config{Codec::H265, hvcCToAnnexB(payload, i + 4)};
            return config.annex_b.empty() ? std::nullopt : std::optional<Config>(std::move(config));
        }
        if (matchesFourCc(payload, i, "avcC"))
        {
            Config config{Codec::H264, avcCToAnnexB(payload, i + 4)};
            return config.annex_b.empty() ? std::nullopt : std::optional<Config>(std::move(config));
        }
    }

    // Bare atom with no fourcc (some phones send avcC as raw bytes).
    if (looksLikeAvcC(payload))
    {
        Config config{Codec::H264, avcCToAnnexB(payload, 0)};
        return config.annex_b.empty() ? std::nullopt : std::optional<Config>(std::move(config));
    }

    Config config{Codec::H265, hvcCToAnnexB(payload, 0)};
    if (config.annex_b.empty())
    {
        SPDLOG_ERROR("[airplay] nalu: no parameter sets found in a {}-byte video config", payload.size());
        return std::nullopt;
    }
    return config;
}

bool isKeyframeNalu(uint8_t header_byte, Codec codec)
{
    if (codec == Codec::H264)
    {
        return (header_byte & 0x1f) == kH264NaluIdr;
    }
    const uint8_t type = static_cast<uint8_t>((header_byte >> 1) & 0x3f);
    return type >= kH265NaluIrapLow && type <= kH265NaluIrapHigh;
}

bool avccContainsKeyframe(const Bytes& frame, Codec codec, size_t length_size)
{
    if (length_size < 1 || length_size > 4)
    {
        SPDLOG_ERROR("[airplay] nalu: bad length prefix size {} (frame {} bytes)", length_size, frame.size());
        return false;
    }

    bool keyframe = false;
    forEachAvccNalu(frame, length_size,
                    [&](size_t offset, size_t)
                    {
                        if (isKeyframeNalu(frame[offset], codec))
                        {
                            keyframe = true;
                        }
                    });
    return keyframe;
}

bool annexBContainsKeyframe(const Bytes& stream, Codec codec)
{
    // Walk 3- and 4-byte start codes; the byte after one is the NAL header.
    for (size_t i = 0; i + 3 < stream.size(); ++i)
    {
        if (stream[i] != 0x00 || stream[i + 1] != 0x00)
        {
            continue;
        }
        if (stream[i + 2] == 0x01)
        {
            if (isKeyframeNalu(stream[i + 3], codec))
            {
                return true;
            }
            i += 2;
        }
        else if (stream[i + 2] == 0x00 && i + 4 < stream.size() && stream[i + 3] == 0x01)
        {
            if (isKeyframeNalu(stream[i + 4], codec))
            {
                return true;
            }
            i += 3;
        }
    }
    return false;
}

}  // namespace airplay::nalu
