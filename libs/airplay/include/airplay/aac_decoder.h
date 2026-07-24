// SPDX-License-Identifier: GPL-3.0-or-later
//
// Decodes the CarPlay entertainment audio stream (type 102), which is AAC-LC
// rather than LPCM, to interleaved S16 PCM using libavcodec.
//
// The phone ships each RTP payload as a *raw* AAC-LC access unit (no ADTS
// header), so the decoder is configured up front with an AudioSpecificConfig
// (from the negotiated sample rate and channel count) and then fed one access
// unit at a time.
#ifndef AIRPLAY_AAC_DECODER_H_
#define AIRPLAY_AAC_DECODER_H_

#include <cstdint>
#include <memory>
#include <vector>

namespace airplay
{

class AacDecoder
{
  public:
    AacDecoder();
    ~AacDecoder();

    AacDecoder(const AacDecoder&) = delete;
    AacDecoder& operator=(const AacDecoder&) = delete;

    // Opens the decoder for AAC-LC at the given rate/channels. Returns false if
    // libavcodec has no AAC decoder or the format is unsupported.
    bool open(uint32_t sample_rate, uint8_t channels);

    // Decodes one raw AAC-LC access unit, appending interleaved S16 PCM to
    // `pcm_out`. Returns false on a decode error (the caller may keep going;
    // one bad packet is not fatal).
    bool decode(const std::vector<uint8_t>& access_unit, std::vector<uint8_t>& pcm_out);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace airplay

#endif  // AIRPLAY_AAC_DECODER_H_
