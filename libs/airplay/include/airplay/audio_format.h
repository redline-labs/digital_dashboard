// SPDX-License-Identifier: GPL-3.0-or-later
//
// CarPlay audio formats. Each is one bit of a mask naming a codec, a sample
// rate and a channel count (all PCM here is 16-bit). /info offers a mask per
// stream type and audio type; SETUP answers with the single bit the phone chose.
//
// One table for both directions, so what is offered and what is decoded cannot
// drift apart: a bit offered here and missing from the decoder is a stream that
// opens and plays nothing.
#ifndef AIRPLAY_AUDIO_FORMAT_H_
#define AIRPLAY_AUDIO_FORMAT_H_

#include <array>
#include <cstdint>
#include <optional>

namespace airplay::audio_format
{

inline constexpr int64_t kPcm8kMono = int64_t{1} << 2;
inline constexpr int64_t kPcm8kStereo = int64_t{1} << 3;
inline constexpr int64_t kPcm16kMono = int64_t{1} << 4;
inline constexpr int64_t kPcm16kStereo = int64_t{1} << 5;
inline constexpr int64_t kPcm24kMono = int64_t{1} << 6;
inline constexpr int64_t kPcm24kStereo = int64_t{1} << 7;
inline constexpr int64_t kPcm32kMono = int64_t{1} << 8;
inline constexpr int64_t kPcm32kStereo = int64_t{1} << 9;
inline constexpr int64_t kPcm44kMono = int64_t{1} << 10;
inline constexpr int64_t kPcm44kStereo = int64_t{1} << 11;
// Bits 12 and 13 are not used here.
inline constexpr int64_t kPcm48kMono = int64_t{1} << 14;
inline constexpr int64_t kPcm48kStereo = int64_t{1} << 15;

inline constexpr int64_t kAacLc44kStereo = int64_t{1} << 22;
inline constexpr int64_t kAacLc48kStereo = int64_t{1} << 23;

// The rates a call or Siri may use, mono, and the same in stereo.
inline constexpr int64_t kPcmVoiceMono = kPcm8kMono | kPcm16kMono | kPcm24kMono | kPcm32kMono;
inline constexpr int64_t kPcmVoice =
    kPcmVoiceMono | kPcm8kStereo | kPcm16kStereo | kPcm24kStereo | kPcm32kStereo;

struct Pcm
{
    int64_t bit;
    uint32_t sample_rate;
    uint8_t channels;
};

inline constexpr std::array<Pcm, 12> kPcmFormats{{
    {kPcm8kMono, 8000, 1},    {kPcm8kStereo, 8000, 2},   {kPcm16kMono, 16000, 1},
    {kPcm16kStereo, 16000, 2}, {kPcm24kMono, 24000, 1},  {kPcm24kStereo, 24000, 2},
    {kPcm32kMono, 32000, 1},  {kPcm32kStereo, 32000, 2}, {kPcm44kMono, 44100, 1},
    {kPcm44kStereo, 44100, 2}, {kPcm48kMono, 48000, 1},  {kPcm48kStereo, 48000, 2},
}};

// The PCM format a SETUP chose, which is exactly one of the bits above.
constexpr std::optional<Pcm> pcm(int64_t chosen)
{
    for (const Pcm& format : kPcmFormats)
    {
        if (format.bit == chosen)
        {
            return format;
        }
    }
    return std::nullopt;
}

}  // namespace airplay::audio_format

#endif  // AIRPLAY_AUDIO_FORMAT_H_
