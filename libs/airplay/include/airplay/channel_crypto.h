// SPDX-License-Identifier: GPL-3.0-or-later
//
// The framed ChaCha20-Poly1305 transport shared by AirPlay's two encrypted
// channels: the control channel, once pair-verify has completed, and the event
// channel, which is encrypted from its first byte.
//
// A frame is a 2-byte little-endian plaintext length, that many ciphertext
// bytes, then a 16-byte tag. The length doubles as the AAD, and each direction
// counts its own nonce from zero -- so the two ends must agree not only on the
// keys but on how many frames have gone by. That is why this is a stateful
// object rather than a pair of free functions over a key: losing a frame, or
// encrypting two frames out of order, desynchronises the counter and every
// later frame fails to authenticate with nothing to say why.
#ifndef AIRPLAY_CHANNEL_CRYPTO_H_
#define AIRPLAY_CHANNEL_CRYPTO_H_

#include <cstdint>
#include <vector>

namespace airplay
{

using Bytes = std::vector<uint8_t>;

class ChannelCrypto
{
  public:
    // Until keys are installed the channel is inactive and traffic is plain.
    // pair-verify M4 is the last plaintext message on the control channel.
    bool active() const { return active_; }

    // `inbound` decrypts what the peer sends; `outbound` encrypts what we send.
    // Which HKDF label maps to which direction differs between the two channels
    // and is the caller's business -- see Receiver for the two conventions,
    // which are *not* the same way round.
    void activate(Bytes inbound, Bytes outbound);
    void deactivate();

    // Pulls as many complete frames out of `cipher` as are available, appending
    // their plaintext to `plain` and erasing what it consumed. A partial frame
    // is left in `cipher` for the next call.
    //
    // Returns false if a frame failed to authenticate, which is fatal for the
    // channel: the counter cannot be resynchronised, so there is nothing to do
    // but close it.
    bool open(Bytes& cipher, Bytes& plain);

    // Encrypts `plain` into one or more frames.
    Bytes seal(const Bytes& plain);

    // AirPlay caps a frame's plaintext at 1024 bytes, so a longer message is
    // split across frames -- each with its own nonce.
    static constexpr size_t kMaxFramePlaintext = 1024;

  private:
    bool active_ = false;
    Bytes inbound_key_;
    Bytes outbound_key_;
    uint64_t inbound_counter_ = 0;
    uint64_t outbound_counter_ = 0;
};

}  // namespace airplay

#endif  // AIRPLAY_CHANNEL_CRYPTO_H_
