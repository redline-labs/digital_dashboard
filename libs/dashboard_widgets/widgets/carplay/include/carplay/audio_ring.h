// SPDX-License-Identifier: GPL-3.0-or-later
//
// A thread-safe pull-mode audio source for QAudioSink.
//
// CarPlay delivers audio in bursts with gaps; feeding those straight to the
// sink underruns between bursts and overruns during them. This decouples the
// two: the network thread pushes decrypted PCM into a ring, and the sink's own
// audio thread pulls samples at the steady sample-clock rate. A short priming
// period builds a cushion before playback starts, and momentary shortfalls are
// filled with silence rather than stalling the sink.
#ifndef CARPLAY_AUDIO_RING_H_
#define CARPLAY_AUDIO_RING_H_

#include <QIODevice>

#include <cstdint>
#include <mutex>
#include <vector>

class AudioRingBuffer : public QIODevice
{
    Q_OBJECT

  public:
    explicit AudioRingBuffer(QObject* parent = nullptr);

    // Sizes the ring and priming threshold for a given format. Clears any
    // buffered audio. capacity_ms bounds latency (oldest audio is dropped past
    // it); prime_ms is how much must accumulate before playback begins.
    void configure(int sample_rate, int channels, int capacity_ms = 500, int prime_ms = 120);

    // Appends PCM (S16LE interleaved) from the network thread.
    void push(const char* data, qint64 len);

    // QIODevice: pull mode. The sink calls readData() on its audio thread.
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;

  protected:
    qint64 readData(char* data, qint64 maxlen) override;
    qint64 writeData(const char*, qint64) override { return -1; }

  private:
    mutable std::mutex _mutex;
    std::vector<char> _ring;
    size_t _head = 0;   // next read position
    size_t _count = 0;  // bytes currently buffered
    size_t _prime_bytes = 0;
    bool _playing = false;  // false while priming or after an underrun

    // Diagnostics.
    uint64_t _underruns = 0;
    uint64_t _overruns = 0;
    uint64_t _push_count = 0;
};

#endif  // CARPLAY_AUDIO_RING_H_
