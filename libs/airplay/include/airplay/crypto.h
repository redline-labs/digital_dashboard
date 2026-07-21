// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/stack/crypto.ts
#ifndef AIRPLAY_CRYPTO_H_
#define AIRPLAY_CRYPTO_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace airplay::crypto
{

using Bytes = std::vector<uint8_t>;

// --- Key agreement / signatures (OpenSSL EVP) ---

struct X25519Pair
{
    Bytes private_key;  // raw 32-byte scalar
    Bytes public_key;   // raw 32-byte point
};

struct Ed25519Pair
{
    Bytes private_key;  // raw 32-byte seed
    Bytes public_key;   // raw 32-byte point
};

X25519Pair x25519Generate();
// Raw 32-byte shared secret; empty on failure.
Bytes x25519Shared(const Bytes& private_key, const Bytes& peer_public_raw);

Ed25519Pair ed25519Generate();
Ed25519Pair ed25519FromSeed(const Bytes& seed);
Bytes ed25519Sign(const Bytes& private_raw, const Bytes& data);
bool ed25519Verify(const Bytes& public_raw, const Bytes& data, const Bytes& signature);

// --- Derivation / hashing ---

// HKDF-SHA512. Labels used by CarPlay include "Pair-Setup-Encrypt-Salt" /
// "Pair-Setup-Encrypt-Info", "Control-Salt", "Events-Salt", etc.
Bytes hkdfSha512(const Bytes& ikm, std::string_view salt, std::string_view info, size_t length);

Bytes sha1(const std::vector<Bytes>& parts);
Bytes sha256(const std::vector<Bytes>& parts);
// SHA-512 over the concatenated parts; used by SRP-6a (pair-setup) and handy
// for the HAP proof hashes, so it lives alongside the other digests.
Bytes sha512(const std::vector<Bytes>& parts);

// --- AEAD / ciphers ---

// ChaCha20-Poly1305 with a 12-byte nonce; seal appends the 16-byte tag.
Bytes chachaSeal(const Bytes& key, const Bytes& nonce, const Bytes& plaintext, const Bytes& aad = {});
// Returns nullopt when authentication fails.
std::optional<Bytes> chachaOpen(const Bytes& key, const Bytes& nonce, const Bytes& ciphertext_with_tag,
                                const Bytes& aad = {});

// 12-byte nonce: 4 zero bytes followed by the little-endian counter.
Bytes nonce64(uint64_t counter);
// 12-byte nonce built from an ASCII label (e.g. "PV-Msg02"), right-aligned.
Bytes nonceLabel(std::string_view label);

// AES-128-CTR (used by MFiSAP /auth-setup).
Bytes aesCtr128(const Bytes& key, const Bytes& iv, const Bytes& data);

// --- Misc ---

Bytes randomBytes(size_t length);
std::string randomId();  // uppercase hex identifier

}  // namespace airplay::crypto

#endif  // AIRPLAY_CRYPTO_H_
