#ifndef AUDIO_TYPE_H_
#define AUDIO_TYPE_H_

#include <string_view>

enum class AudioFrequency
{
    Frequency_48000,
    Frequency_44100,
    Frequency_24000,
    Frequency_16000,
    Frequency_8000
};

constexpr std::string_view audio_frequency_to_string(AudioFrequency frequency)
{
    switch (frequency)
    {
        case (AudioFrequency::Frequency_48000):
            return "48000 Hz";

        case (AudioFrequency::Frequency_44100):
            return "44100 Hz";

        case (AudioFrequency::Frequency_24000):
            return "24000 Hz";

        case (AudioFrequency::Frequency_16000):
            return "16000 Hz";

        case (AudioFrequency::Frequency_8000):
            return "8000 Hz";

        default:
            return "Unknown";
    }
}


enum class AudioChannel
{
    Channel_1,
    Channel_2
};

constexpr std::string_view audio_class_frequency_to_string(AudioChannel channel)
{
    switch (channel)
    {
        case (AudioChannel::Channel_1):
            return "Channel 1";

        case (AudioChannel::Channel_2):
            return "Channel 2";

        default:
            return "Unknown";
    }
}


struct AudioFormat
{
    constexpr AudioFormat():
        frequency{AudioFrequency::Frequency_48000},
        channel{AudioChannel::Channel_1},
        bitrate_kbps{16}
    {
    }

    constexpr AudioFormat(AudioFrequency in_frequency, AudioChannel in_channel, uint16_t in_bitrate_kbps):
        frequency{in_frequency},
        channel{in_channel},
        bitrate_kbps{in_bitrate_kbps}
    {
    }

    AudioFrequency frequency;
    AudioChannel channel;
    uint16_t bitrate_kbps;
};

constexpr AudioFormat decode_audio_type(uint8_t audio_type)
{
    switch (audio_type)
    {
        case (1):
            return AudioFormat(AudioFrequency::Frequency_44100, AudioChannel::Channel_2, 16);

        case (2):
            return AudioFormat(AudioFrequency::Frequency_44100, AudioChannel::Channel_2, 16);

        case (3):
            return AudioFormat(AudioFrequency::Frequency_8000, AudioChannel::Channel_1, 16);

        case (4):
            return AudioFormat(AudioFrequency::Frequency_48000, AudioChannel::Channel_2, 16);

        case (5):
            return AudioFormat(AudioFrequency::Frequency_16000, AudioChannel::Channel_1, 16);

        case (6):
            return AudioFormat(AudioFrequency::Frequency_24000, AudioChannel::Channel_1, 16);;

        case (7):
        default:
            return AudioFormat(AudioFrequency::Frequency_16000, AudioChannel::Channel_2, 16);
    }
}

#endif  // AUDIO_TYPE_H_