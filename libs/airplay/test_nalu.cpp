// SPDX-License-Identifier: GPL-3.0-or-later
// avcC/hvcC -> Annex-B rewriting, codec detection and keyframe detection.
#include "airplay/nalu.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string>

namespace
{

using airplay::nalu::Bytes;
using airplay::nalu::Codec;

int failures = 0;

void expect(bool condition, const char* what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

int hexDigit(char c)
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

// Whitespace in the literals below is ignored so the atoms stay readable.
Bytes fromHex(std::string_view hex)
{
    Bytes out;
    out.reserve(hex.size() / 2);
    int high = -1;
    for (char c : hex)
    {
        const int digit = hexDigit(c);
        if (digit < 0)
        {
            continue;
        }
        if (high < 0)
        {
            high = digit;
        }
        else
        {
            out.push_back(static_cast<uint8_t>((high << 4) | digit));
            high = -1;
        }
    }
    return out;
}

std::string toHex(const Bytes& bytes)
{
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes)
    {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0f]);
    }
    return out;
}

struct QuietLogs
{
    QuietLogs() { spdlog::set_level(spdlog::level::off); }
    ~QuietLogs() { spdlog::set_level(spdlog::level::info); }
};

void appendFourCc(Bytes& out, const char* cc)
{
    for (size_t i = 0; i < 4; ++i)
    {
        out.push_back(static_cast<uint8_t>(cc[i]));
    }
}

// A minimal but structurally valid avcC record: SPS (NAL type 7) and PPS
// (NAL type 8).
Bytes makeAvcCRecord()
{
    return fromHex(
        "01"      // configurationVersion
        "42c01e"  // profile, compatibility, level
        "ff"      // 0xfc | lengthSizeMinusOne = 3
        "e1"      // 0xe0 | numOfSequenceParameterSets = 1
        "0009"    // SPS length
        "6742c01e d900b0c9"  // SPS payload (NAL header 0x67 -> type 7)
        "22"
        "01"    // numOfPictureParameterSets
        "0004"  // PPS length
        "68ce3c80");
}

// A minimal but structurally valid hvcC record: 22 fixed bytes, then VPS/SPS/PPS
// arrays.
Bytes makeHvcCRecord()
{
    // 22 fixed bytes, then numOfArrays.
    Bytes record = fromHex(
        "01"            // configurationVersion
        "01"            // general_profile_space / tier / idc
        "60000000"      // general_profile_compatibility_flags
        "000000000000"  // general_constraint_indicator_flags
        "99"            // general_level_idc
        "f000"          // min_spatial_segmentation_idc
        "fc"            // parallelismType
        "fd"            // chromaFormat
        "f8"            // bitDepthLumaMinus8
        "f8"            // bitDepthChromaMinus8
        "0000"          // avgFrameRate
        "0f"            // constantFrameRate .. lengthSizeMinusOne
        "03");          // numOfArrays

    // VPS array: NAL type 32.
    record.insert(record.end(), {0x20, 0x00, 0x01, 0x00, 0x04});
    record.insert(record.end(), {0x40, 0x01, 0x0c, 0x01});
    // SPS array: NAL type 33.
    record.insert(record.end(), {0x21, 0x00, 0x01, 0x00, 0x05});
    record.insert(record.end(), {0x42, 0x01, 0x01, 0x01, 0x60});
    // PPS array: NAL type 34.
    record.insert(record.end(), {0x22, 0x00, 0x01, 0x00, 0x03});
    record.insert(record.end(), {0x44, 0x01, 0xc1});
    return record;
}

void testAvccFrameRewrite()
{
    using airplay::nalu::avccFrameToAnnexB;

    // Two 4-byte length prefixed NAL units.
    const Bytes frame = fromHex(
        "00000003"
        "65aabb"
        "00000004"
        "41ccddee");
    expect(toHex(avccFrameToAnnexB(frame)) == "0000000165aabb0000000141ccddee",
           "avcC frame -> Annex-B (two NAL units)");

    // Single NAL unit.
    expect(toHex(avccFrameToAnnexB(fromHex("0000000267aa"))) == "0000000167aa",
           "avcC frame -> Annex-B (single NAL unit)");

    // A 2-byte length prefix.
    expect(toHex(avccFrameToAnnexB(fromHex("000267aa" "000268bb"), 2)) == "0000000167aa" "0000000168bb",
           "avcC frame -> Annex-B with a 2-byte length prefix");

    // A 1-byte length prefix.
    expect(toHex(avccFrameToAnnexB(fromHex("0267aa"), 1)) == "0000000167aa",
           "avcC frame -> Annex-B with a 1-byte length prefix");

    // Trailing garbage that cannot be a NAL unit is dropped, like the original.
    expect(toHex(avccFrameToAnnexB(fromHex("0000000267aa" "000000ff41"))) == "0000000167aa",
           "avcC frame -> Annex-B stops at a length that overruns the buffer");

    expect(avccFrameToAnnexB({}).empty(), "empty frame yields nothing");
    expect(avccFrameToAnnexB(fromHex("000000")).empty(), "runt frame yields nothing");
    expect(avccFrameToAnnexB(fromHex("00000000")).empty(), "zero-length NAL unit yields nothing");

    const QuietLogs quiet;
    expect(avccFrameToAnnexB(frame, 0).empty(), "length prefix size 0 is rejected");
    expect(avccFrameToAnnexB(frame, 5).empty(), "length prefix size 5 is rejected");
}

void testConfigToAnnexB()
{
    using airplay::nalu::configToAnnexB;

    const std::string expected_avc =
        "00000001"
        "6742c01ed900b0c922"
        "00000001"
        "68ce3c80";

    // Bare avcC record with no fourcc.
    {
        const auto config = configToAnnexB(makeAvcCRecord());
        expect(config.has_value(), "bare avcC record is recognised");
        if (config)
        {
            expect(config->codec == Codec::H264, "bare avcC record detects H.264");
            expect(toHex(config->annex_b) == expected_avc, "bare avcC record yields SPS + PPS");
        }
    }

    // avcC box: [size]['avcC'][record].
    {
        Bytes payload = fromHex("00000000");
        appendFourCc(payload, "avcC");
        const Bytes record = makeAvcCRecord();
        payload.insert(payload.end(), record.begin(), record.end());
        payload[3] = static_cast<uint8_t>(payload.size());

        const auto config = configToAnnexB(payload);
        expect(config.has_value(), "avcC box is recognised");
        if (config)
        {
            expect(config->codec == Codec::H264, "avcC box detects H.264");
            expect(toHex(config->annex_b) == expected_avc, "avcC box yields SPS + PPS");
        }
    }

    const std::string expected_hevc =
        "00000001"
        "40010c01"
        "00000001"
        "4201010160"
        "00000001"
        "4401c1";

    // Bare hvcC record (no fourcc, and it does not look like an avcC).
    {
        const auto config = configToAnnexB(makeHvcCRecord());
        expect(config.has_value(), "bare hvcC record is recognised");
        if (config)
        {
            expect(config->codec == Codec::H265, "bare hvcC record detects H.265");
            expect(toHex(config->annex_b) == expected_hevc, "bare hvcC record yields VPS + SPS + PPS");
        }
    }

    // hvc1 sample entry with the hvcC box nested inside, which is how H.265
    // actually arrives from the phone.
    {
        Bytes payload = fromHex("0000006e");
        appendFourCc(payload, "hvc1");
        payload.resize(payload.size() + 78, 0x00);  // sample entry fixed fields
        Bytes inner = fromHex("00000000");
        appendFourCc(inner, "hvcC");
        const Bytes record = makeHvcCRecord();
        inner.insert(inner.end(), record.begin(), record.end());
        inner[3] = static_cast<uint8_t>(inner.size());
        payload.insert(payload.end(), inner.begin(), inner.end());

        const auto config = configToAnnexB(payload);
        expect(config.has_value(), "nested hvcC inside hvc1 is recognised");
        if (config)
        {
            expect(config->codec == Codec::H265, "nested hvcC detects H.265");
            expect(toHex(config->annex_b) == expected_hevc, "nested hvcC yields VPS + SPS + PPS");
        }
    }

    // The fourcc search must win over the bare-atom heuristic.
    {
        Bytes payload = fromHex("00000000");
        appendFourCc(payload, "hvcC");
        const Bytes record = makeHvcCRecord();
        payload.insert(payload.end(), record.begin(), record.end());
        const auto config = configToAnnexB(payload);
        expect(config.has_value() && config->codec == Codec::H265, "hvcC fourcc beats the avcC heuristic");
    }

    const QuietLogs quiet;
    expect(!configToAnnexB({}).has_value(), "empty config is rejected");
    expect(!configToAnnexB(fromHex("00112233445566778899")).has_value(), "garbage config is rejected");
    // Truncated avcC: the SPS length runs past the end.
    expect(!configToAnnexB(fromHex("0142c01effe100ff67")).has_value(), "truncated avcC is rejected");
}

void testKeyframeDetection()
{
    using airplay::nalu::annexBContainsKeyframe;
    using airplay::nalu::avccContainsKeyframe;
    using airplay::nalu::isKeyframeNalu;

    // H.264 NAL header bytes: 0x65 is an IDR slice, 0x41 a non-IDR slice.
    expect(isKeyframeNalu(0x65, Codec::H264), "H.264 IDR NAL header (0x65)");
    expect(isKeyframeNalu(0x25, Codec::H264), "H.264 IDR NAL header with nal_ref_idc 1 (0x25)");
    expect(!isKeyframeNalu(0x41, Codec::H264), "H.264 non-IDR slice is not a keyframe");
    expect(!isKeyframeNalu(0x67, Codec::H264), "H.264 SPS alone is not a keyframe");
    expect(!isKeyframeNalu(0x68, Codec::H264), "H.264 PPS alone is not a keyframe");

    // H.265 NAL types live in bits 6..1: IDR_W_RADL is 19 -> 0x26, CRA is 21 ->
    // 0x2a, TRAIL_R is 1 -> 0x02.
    expect(isKeyframeNalu(0x26, Codec::H265), "H.265 IDR_W_RADL (type 19)");
    expect(isKeyframeNalu(0x28, Codec::H265), "H.265 IDR_N_LP (type 20)");
    expect(isKeyframeNalu(0x2a, Codec::H265), "H.265 CRA_NUT (type 21)");
    expect(isKeyframeNalu(0x20, Codec::H265), "H.265 BLA_W_LP (type 16) is IRAP");
    expect(isKeyframeNalu(0x2e, Codec::H265), "H.265 RSV_IRAP_VCL23 (type 23) is IRAP");
    expect(!isKeyframeNalu(0x02, Codec::H265), "H.265 TRAIL_R is not a keyframe");
    expect(!isKeyframeNalu(0x30, Codec::H265), "H.265 type 24 is past the IRAP range");
    expect(!isKeyframeNalu(0x40, Codec::H265), "H.265 VPS alone is not a keyframe");

    // Length-prefixed access units.
    const Bytes idr_access_unit = fromHex(
        "00000009"
        "6742c01ed900b0c922"
        "00000004"
        "68ce3c80"
        "00000005"
        "65aabbccdd");
    const Bytes inter_access_unit = fromHex(
        "00000005"
        "41aabbccdd"
        "00000003"
        "41eeff00");

    expect(avccContainsKeyframe(idr_access_unit, Codec::H264), "avcC access unit with an IDR is a keyframe");
    expect(!avccContainsKeyframe(inter_access_unit, Codec::H264), "avcC access unit without an IDR is not");
    expect(!avccContainsKeyframe({}, Codec::H264), "empty access unit is not a keyframe");

    const Bytes hevc_irap = fromHex(
        "00000004"
        "40010c01"
        "00000006"
        "2601aabbccdd");
    const Bytes hevc_inter = fromHex(
        "00000006"
        "0201aabbccdd");
    expect(avccContainsKeyframe(hevc_irap, Codec::H265), "hvcC access unit with an IRAP is a keyframe");
    expect(!avccContainsKeyframe(hevc_inter, Codec::H265), "hvcC access unit without an IRAP is not");
    expect(!avccContainsKeyframe(hevc_irap, Codec::H264), "H.265 IRAP is not mistaken for an H.264 IDR");

    // The same access units after the Annex-B rewrite.
    expect(annexBContainsKeyframe(airplay::nalu::avccFrameToAnnexB(idr_access_unit), Codec::H264),
           "Annex-B keyframe detection after rewriting");
    expect(!annexBContainsKeyframe(airplay::nalu::avccFrameToAnnexB(inter_access_unit), Codec::H264),
           "Annex-B non-keyframe detection after rewriting");
    expect(annexBContainsKeyframe(airplay::nalu::avccFrameToAnnexB(hevc_irap), Codec::H265),
           "Annex-B H.265 keyframe detection after rewriting");

    // 3-byte start codes must be handled too.
    expect(annexBContainsKeyframe(fromHex("00000141aa" "00000165bb"), Codec::H264),
           "Annex-B 3-byte start codes are handled");
    expect(!annexBContainsKeyframe(fromHex("00000141aa"), Codec::H264), "Annex-B non-keyframe with 3-byte codes");
    expect(!annexBContainsKeyframe({}, Codec::H264), "empty Annex-B stream is not a keyframe");

    const QuietLogs quiet;
    expect(!avccContainsKeyframe(idr_access_unit, Codec::H264, 7), "bad length prefix size is rejected");
}

// Config atoms and frames come straight off the wire, so mutate valid ones and
// confirm nothing crashes or reads out of bounds. Deterministic seed.
void testMutationFuzz()
{
    const QuietLogs quiet;

    const Bytes seeds[] = {makeAvcCRecord(), makeHvcCRecord(),
                           fromHex("00000009 6742c01ed900b0c922 00000004 68ce3c80 00000005 65aabbccdd")};

    uint32_t state = 0xfeedface;
    const auto next = [&state]
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };

    size_t results = 0;
    for (size_t i = 0; i < 20000; ++i)
    {
        Bytes mutated = seeds[next() % 3];
        const size_t edits = 1 + (next() % 4);
        for (size_t e = 0; e < edits; ++e)
        {
            mutated[next() % mutated.size()] = static_cast<uint8_t>(next() & 0xff);
        }
        if (next() % 4 == 0)
        {
            mutated.resize(1 + (next() % mutated.size()));
        }
        // Only the absence of a crash matters; any outcome is legal.
        results += airplay::nalu::configToAnnexB(mutated).has_value() ? 1 : 0;
        results += airplay::nalu::avccFrameToAnnexB(mutated, 1 + (next() % 4)).size();
        results += airplay::nalu::avccContainsKeyframe(mutated, Codec::H264) ? 1 : 0;
        results += airplay::nalu::annexBContainsKeyframe(mutated, Codec::H265) ? 1 : 0;
    }
    expect(results < SIZE_MAX, "mutation fuzz survived without crashing");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    testAvccFrameRewrite();
    testConfigToAnnexB();
    testKeyframeDetection();
    testMutationFuzz();

    if (failures == 0)
    {
        SPDLOG_INFO("nalu tests passed");
        return EXIT_SUCCESS;
    }
    SPDLOG_ERROR("{} nalu test(s) failed", failures);
    return EXIT_FAILURE;
}
