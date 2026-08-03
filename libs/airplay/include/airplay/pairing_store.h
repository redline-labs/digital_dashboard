// SPDX-License-Identifier: GPL-3.0-or-later
//
// What has to survive a restart for a phone to stay paired: the accessory's own
// long-term identity, and the long-term public key of every phone that has
// completed pair-setup with it.
//
// What this does NOT do, on the wired path: stop the phone re-pairing. Measured
// on hardware 2026-08-02 -- with a stable identity and the phone's key on file,
// the phone still ran a full pair-setup on the next session. It sends
// `X-Apple-HKP: 0`, which is *transient* pairing, and it does that because wired
// CarPlay has no Bonjour advertisement carrying our pairing id and public key,
// so it has nothing to recognise us by before it connects. LIVI persists these
// for its wireless path, where those do appear in the TXT records.
//
// What it does do, and why it is worth having anyway:
//
//   - The accessory identity stops changing on every restart, which is correct
//     in itself and a prerequisite for ever offering wireless.
//   - It makes pair-verify *enforceable*. The M3 signature is checked against
//     the phone's stored public key; with no store the only key available is
//     the one from the same session's pair-setup, which proves nothing about
//     continuity and made the check ceremonial. It is now refused when it does
//     not match -- verified on hardware as "verified against the stored key".
//
// The files hold private key material and are written 0600. A missing or
// unreadable store is not an error -- it means "not paired yet" and the next
// pair-setup rebuilds it -- but a store that exists and cannot be parsed is
// left alone rather than overwritten, so a bad read does not silently discard
// every pairing on the vehicle.
#ifndef AIRPLAY_PAIRING_STORE_H_
#define AIRPLAY_PAIRING_STORE_H_

#include "airplay/crypto.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace airplay
{

using Bytes = std::vector<uint8_t>;

class PairingStore
{
  public:
    // `directory` holds the two files. Empty disables persistence entirely,
    // which is what a test or a bring-up session that wants a clean slate uses.
    explicit PairingStore(std::string directory);

    bool enabled() const { return !directory_.empty(); }

    // The accessory's long-term identity, loaded if present and generated and
    // saved if not. With persistence disabled this generates a fresh one every
    // call, which is the old behaviour.
    crypto::Ed25519Pair loadOrCreateIdentity();

    // The long-term public key a phone handed over during pair-setup, or
    // nullopt if this phone has never paired with us.
    std::optional<Bytes> phoneKey(const std::string& identifier) const;

    // Records a phone's key. Overwrites any previous key for the same
    // identifier: a phone that re-runs pair-setup has rotated its key, and the
    // new one is the one that will sign.
    void savePhoneKey(const std::string& identifier, const Bytes& ltpk);

    // Number of phones on file, for logging and tests.
    size_t phoneCount() const;

  private:
    std::string identityPath() const;
    std::string phonesPath() const;

    std::string directory_;
};

}  // namespace airplay

#endif  // AIRPLAY_PAIRING_STORE_H_
