// SPDX-License-Identifier: GPL-3.0-or-later
#include "carplay/audio_ring.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

AudioRingBuffer::AudioRingBuffer(QObject* parent) : QIODevice(parent) {}

void AudioRingBuffer::configure(int sample_rate, int channels, int capacity_ms, int prime_ms)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const size_t bytes_per_second =
        static_cast<size_t>(sample_rate) * static_cast<size_t>(channels) * 2u;  // S16
    _ring.assign((bytes_per_second * static_cast<size_t>(capacity_ms)) / 1000u, 0);
    _prime_bytes = (bytes_per_second * static_cast<size_t>(prime_ms)) / 1000u;
    _head = 0;
    _count = 0;
    _playing = false;
    _underruns = 0;
    _overruns = 0;
}

void AudioRingBuffer::push(const char* data, qint64 len)
{
    if (len <= 0)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    const size_t cap = _ring.size();
    if (cap == 0)
    {
        return;
    }
    size_t n = static_cast<size_t>(len);

    // Bound latency: if the incoming data would exceed capacity, keep only the
    // most recent capacity-worth (drop the oldest). This drops audio only when
    // the sink has fallen badly behind, which is preferable to unbounded lag.
    if (n > cap)
    {
        data += (n - cap);
        n = cap;
    }
    if (_count + n > cap)
    {
        const size_t drop = _count + n - cap;
        _head = (_head + drop) % cap;
        _count -= drop;
        ++_overruns;
    }

    const size_t tail = (_head + _count) % cap;
    const size_t first = std::min(n, cap - tail);
    std::memcpy(_ring.data() + tail, data, first);
    if (n > first)
    {
        std::memcpy(_ring.data(), data + first, n - first);
    }
    _count += n;

    // Periodic health line (~once/sec at 134 packets/s). Growing overruns with
    // zero underruns means the audio device is draining slower than real time --
    // a host/VM audio problem, not a buffering one (verified: a VMware emulated
    // audio device under CPU contention stutters every source, aplay included).
    if (++_push_count % 134 == 0)
    {
        SPDLOG_DEBUG("[carplay] audio ring: fill {}% ({}/{} B), underruns {}, overruns {}",
                     cap ? (_count * 100 / cap) : 0, _count, cap, _underruns, _overruns);
    }
}

qint64 AudioRingBuffer::bytesAvailable() const
{
    // Report a large, always-positive figure so the sink keeps pulling even
    // while the ring is momentarily empty (readData fills silence). The base
    // class adds its internal buffer, which is empty here.
    return (1 << 20) + QIODevice::bytesAvailable();
}

qint64 AudioRingBuffer::readData(char* data, qint64 maxlen)
{
    if (maxlen <= 0)
    {
        return 0;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    const size_t cap = _ring.size();
    const size_t want = static_cast<size_t>(maxlen);

    // While priming (or re-priming after an underrun) play silence and let the
    // ring fill to the cushion first.
    if (!_playing)
    {
        if (_count < _prime_bytes)
        {
            std::memset(data, 0, want);
            return maxlen;
        }
        _playing = true;
    }

    const size_t take = std::min(want, _count);
    if (cap != 0 && take != 0)
    {
        const size_t first = std::min(take, cap - _head);
        std::memcpy(data, _ring.data() + _head, first);
        if (take > first)
        {
            std::memcpy(data + first, _ring.data(), take - first);
        }
        _head = (_head + take) % cap;
        _count -= take;
    }

    // Underran within this read: fill the rest with silence and re-prime so the
    // next gap rebuilds the cushion instead of stuttering sample-by-sample.
    if (take < want)
    {
        std::memset(data + take, 0, want - take);
        if (_count == 0)
        {
            _playing = false;
            ++_underruns;
        }
    }
    return maxlen;
}
