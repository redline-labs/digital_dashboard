// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/stack/crypto.ts
#include "airplay/crypto.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <memory>

namespace airplay::crypto
{

namespace
{

constexpr size_t kX25519KeySize = 32;
constexpr size_t kEd25519KeySize = 32;
constexpr size_t kEd25519SignatureSize = 64;
constexpr size_t kChachaKeySize = 32;
constexpr size_t kChachaNonceSize = 12;
constexpr size_t kChachaTagSize = 16;
constexpr size_t kAes128KeySize = 16;
constexpr size_t kAes128BlockSize = 16;

// RAII helpers so error paths never leak OpenSSL handles.
struct PkeyDeleter
{
    void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
};
struct PkeyCtxDeleter
{
    void operator()(EVP_PKEY_CTX* p) const { EVP_PKEY_CTX_free(p); }
};
struct MdCtxDeleter
{
    void operator()(EVP_MD_CTX* p) const { EVP_MD_CTX_free(p); }
};
struct CipherCtxDeleter
{
    void operator()(EVP_CIPHER_CTX* p) const { EVP_CIPHER_CTX_free(p); }
};

using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDeleter>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, PkeyCtxDeleter>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, MdCtxDeleter>;
using CipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, CipherCtxDeleter>;

Bytes generateRawPair(int type, Bytes& public_out, const char* what)
{
    PkeyCtxPtr ctx(EVP_PKEY_CTX_new_id(type, nullptr));
    if (!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0)
    {
        SPDLOG_ERROR("[airplay] {}: keygen init failed", what);
        return {};
    }

    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(ctx.get(), &raw) <= 0)
    {
        SPDLOG_ERROR("[airplay] {}: keygen failed", what);
        return {};
    }
    PkeyPtr key(raw);

    size_t private_length = 0;
    size_t public_length = 0;
    if (EVP_PKEY_get_raw_private_key(key.get(), nullptr, &private_length) <= 0 ||
        EVP_PKEY_get_raw_public_key(key.get(), nullptr, &public_length) <= 0)
    {
        SPDLOG_ERROR("[airplay] {}: raw key size query failed", what);
        return {};
    }

    Bytes private_key(private_length);
    public_out.assign(public_length, 0);
    if (EVP_PKEY_get_raw_private_key(key.get(), private_key.data(), &private_length) <= 0 ||
        EVP_PKEY_get_raw_public_key(key.get(), public_out.data(), &public_length) <= 0)
    {
        SPDLOG_ERROR("[airplay] {}: raw key export failed", what);
        public_out.clear();
        return {};
    }

    private_key.resize(private_length);
    public_out.resize(public_length);
    return private_key;
}

Bytes digest(const EVP_MD* md, const std::vector<Bytes>& parts, const char* what)
{
    MdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx || EVP_DigestInit_ex(ctx.get(), md, nullptr) != 1)
    {
        SPDLOG_ERROR("[airplay] {}: digest init failed", what);
        return {};
    }

    for (const auto& part : parts)
    {
        if (part.empty())
        {
            continue;
        }
        if (EVP_DigestUpdate(ctx.get(), part.data(), part.size()) != 1)
        {
            SPDLOG_ERROR("[airplay] {}: digest update failed (part {} bytes)", what, part.size());
            return {};
        }
    }

    Bytes out(static_cast<size_t>(EVP_MD_get_size(md)));
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(ctx.get(), out.data(), &length) != 1)
    {
        SPDLOG_ERROR("[airplay] {}: digest final failed", what);
        return {};
    }
    out.resize(length);
    return out;
}

// One-shot HMAC-SHA512 over a pre-concatenated buffer.
Bytes hmacSha512(const uint8_t* key, size_t key_length, const uint8_t* data, size_t data_length)
{
    Bytes out(EVP_MAX_MD_SIZE);
    unsigned int length = 0;
    // OpenSSL rejects a null key pointer even when the length is zero.
    const uint8_t empty = 0;
    const uint8_t* key_ptr = (key_length == 0) ? &empty : key;
    if (HMAC(EVP_sha512(), key_ptr, static_cast<int>(key_length), data, data_length, out.data(), &length) == nullptr)
    {
        SPDLOG_ERROR("[airplay] hmacSha512: failed (key {} bytes, data {} bytes)", key_length, data_length);
        return {};
    }
    out.resize(length);
    return out;
}

}  // namespace

// --- X25519 -----------------------------------------------------------------

X25519Pair x25519Generate()
{
    X25519Pair pair;
    pair.private_key = generateRawPair(EVP_PKEY_X25519, pair.public_key, "x25519Generate");
    return pair;
}

Bytes x25519Shared(const Bytes& private_key, const Bytes& peer_public_raw)
{
    if (private_key.size() != kX25519KeySize || peer_public_raw.size() != kX25519KeySize)
    {
        SPDLOG_ERROR("[airplay] x25519Shared: bad key sizes (private {} bytes, peer {} bytes, expected {})",
                     private_key.size(), peer_public_raw.size(), kX25519KeySize);
        return {};
    }

    PkeyPtr self(EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, private_key.data(), private_key.size()));
    PkeyPtr peer(EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, peer_public_raw.data(), peer_public_raw.size()));
    if (!self || !peer)
    {
        SPDLOG_ERROR("[airplay] x25519Shared: raw key import failed");
        return {};
    }

    PkeyCtxPtr ctx(EVP_PKEY_CTX_new(self.get(), nullptr));
    if (!ctx || EVP_PKEY_derive_init(ctx.get()) <= 0 || EVP_PKEY_derive_set_peer(ctx.get(), peer.get()) <= 0)
    {
        SPDLOG_ERROR("[airplay] x25519Shared: derive init failed");
        return {};
    }

    size_t length = 0;
    if (EVP_PKEY_derive(ctx.get(), nullptr, &length) <= 0)
    {
        SPDLOG_ERROR("[airplay] x25519Shared: derive size query failed");
        return {};
    }

    Bytes secret(length);
    if (EVP_PKEY_derive(ctx.get(), secret.data(), &length) <= 0)
    {
        // A shared secret of all zeros (low-order peer point) also lands here.
        SPDLOG_ERROR("[airplay] x25519Shared: derive failed");
        return {};
    }
    secret.resize(length);
    return secret;
}

// --- Ed25519 ----------------------------------------------------------------

Ed25519Pair ed25519Generate()
{
    Ed25519Pair pair;
    pair.private_key = generateRawPair(EVP_PKEY_ED25519, pair.public_key, "ed25519Generate");
    return pair;
}

Ed25519Pair ed25519FromSeed(const Bytes& seed)
{
    Ed25519Pair pair;
    if (seed.size() != kEd25519KeySize)
    {
        SPDLOG_ERROR("[airplay] ed25519FromSeed: bad seed size ({} bytes, expected {})", seed.size(), kEd25519KeySize);
        return pair;
    }

    PkeyPtr key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()));
    if (!key)
    {
        SPDLOG_ERROR("[airplay] ed25519FromSeed: raw key import failed");
        return pair;
    }

    size_t length = kEd25519KeySize;
    Bytes public_key(length);
    if (EVP_PKEY_get_raw_public_key(key.get(), public_key.data(), &length) <= 0)
    {
        SPDLOG_ERROR("[airplay] ed25519FromSeed: public key export failed");
        return pair;
    }
    public_key.resize(length);

    pair.private_key = seed;
    pair.public_key = std::move(public_key);
    return pair;
}

Bytes ed25519Sign(const Bytes& private_raw, const Bytes& data)
{
    if (private_raw.size() != kEd25519KeySize)
    {
        SPDLOG_ERROR("[airplay] ed25519Sign: bad key size ({} bytes, expected {})", private_raw.size(),
                     kEd25519KeySize);
        return {};
    }

    PkeyPtr key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, private_raw.data(), private_raw.size()));
    MdCtxPtr ctx(EVP_MD_CTX_new());
    if (!key || !ctx)
    {
        SPDLOG_ERROR("[airplay] ed25519Sign: context allocation failed");
        return {};
    }

    if (EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1)
    {
        SPDLOG_ERROR("[airplay] ed25519Sign: sign init failed (data {} bytes)", data.size());
        return {};
    }

    size_t length = kEd25519SignatureSize;
    Bytes signature(length);
    if (EVP_DigestSign(ctx.get(), signature.data(), &length, data.data(), data.size()) != 1)
    {
        SPDLOG_ERROR("[airplay] ed25519Sign: sign failed (data {} bytes)", data.size());
        return {};
    }
    signature.resize(length);
    return signature;
}

bool ed25519Verify(const Bytes& public_raw, const Bytes& data, const Bytes& signature)
{
    if (public_raw.size() != kEd25519KeySize || signature.size() != kEd25519SignatureSize)
    {
        SPDLOG_ERROR("[airplay] ed25519Verify: bad sizes (key {} bytes, signature {} bytes)", public_raw.size(),
                     signature.size());
        return false;
    }

    PkeyPtr key(EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, public_raw.data(), public_raw.size()));
    MdCtxPtr ctx(EVP_MD_CTX_new());
    if (!key || !ctx)
    {
        SPDLOG_ERROR("[airplay] ed25519Verify: context allocation failed");
        return false;
    }

    if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1)
    {
        SPDLOG_ERROR("[airplay] ed25519Verify: verify init failed (data {} bytes)", data.size());
        return false;
    }

    return EVP_DigestVerify(ctx.get(), signature.data(), signature.size(), data.data(), data.size()) == 1;
}

// --- HKDF / hashes ----------------------------------------------------------

Bytes hkdfSha512(const Bytes& ikm, std::string_view salt, std::string_view info, size_t length)
{
    // RFC 5869 by hand: OpenSSL's EVP_KDF wrapper has historically refused a
    // zero-length salt, which CarPlay does use for a couple of derivations.
    constexpr size_t kHashLength = 64;
    if (length == 0 || length > 255 * kHashLength)
    {
        SPDLOG_ERROR("[airplay] hkdfSha512: bad output length ({} bytes)", length);
        return {};
    }

    const auto* salt_ptr = reinterpret_cast<const uint8_t*>(salt.data());
    const Bytes prk = hmacSha512(salt_ptr, salt.size(), ikm.data(), ikm.size());
    if (prk.size() != kHashLength)
    {
        SPDLOG_ERROR("[airplay] hkdfSha512: extract failed (ikm {} bytes, salt {} bytes)", ikm.size(), salt.size());
        return {};
    }

    Bytes out;
    out.reserve(length);
    Bytes block;
    Bytes input;
    for (uint8_t counter = 1; out.size() < length; ++counter)
    {
        input.clear();
        input.insert(input.end(), block.begin(), block.end());
        input.insert(input.end(), info.begin(), info.end());
        input.push_back(counter);

        block = hmacSha512(prk.data(), prk.size(), input.data(), input.size());
        if (block.size() != kHashLength)
        {
            SPDLOG_ERROR("[airplay] hkdfSha512: expand failed at block {}", counter);
            return {};
        }

        const size_t take = std::min(block.size(), length - out.size());
        out.insert(out.end(), block.begin(), block.begin() + static_cast<ptrdiff_t>(take));
    }
    return out;
}

Bytes sha1(const std::vector<Bytes>& parts)
{
    return digest(EVP_sha1(), parts, "sha1");
}

Bytes sha256(const std::vector<Bytes>& parts)
{
    return digest(EVP_sha256(), parts, "sha256");
}

Bytes sha512(const std::vector<Bytes>& parts)
{
    return digest(EVP_sha512(), parts, "sha512");
}

// --- ChaCha20-Poly1305 ------------------------------------------------------

Bytes chachaSeal(const Bytes& key, const Bytes& nonce, const Bytes& plaintext, const Bytes& aad)
{
    if (key.size() != kChachaKeySize || nonce.size() != kChachaNonceSize)
    {
        SPDLOG_ERROR("[airplay] chachaSeal: bad sizes (key {} bytes, nonce {} bytes)", key.size(), nonce.size());
        return {};
    }

    CipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(kChachaNonceSize), nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce.data()) != 1)
    {
        SPDLOG_ERROR("[airplay] chachaSeal: init failed (plaintext {} bytes, aad {} bytes)", plaintext.size(),
                     aad.size());
        return {};
    }

    int length = 0;
    if (!aad.empty() &&
        EVP_EncryptUpdate(ctx.get(), nullptr, &length, aad.data(), static_cast<int>(aad.size())) != 1)
    {
        SPDLOG_ERROR("[airplay] chachaSeal: aad update failed ({} bytes)", aad.size());
        return {};
    }

    Bytes out(plaintext.size() + kChachaTagSize);
    if (!plaintext.empty() &&
        EVP_EncryptUpdate(ctx.get(), out.data(), &length, plaintext.data(), static_cast<int>(plaintext.size())) != 1)
    {
        SPDLOG_ERROR("[airplay] chachaSeal: encrypt failed ({} bytes)", plaintext.size());
        return {};
    }

    int final_length = 0;
    if (EVP_EncryptFinal_ex(ctx.get(), out.data() + plaintext.size(), &final_length) != 1)
    {
        SPDLOG_ERROR("[airplay] chachaSeal: finalize failed ({} bytes)", plaintext.size());
        return {};
    }

    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_GET_TAG, static_cast<int>(kChachaTagSize),
                            out.data() + plaintext.size()) != 1)
    {
        SPDLOG_ERROR("[airplay] chachaSeal: tag extraction failed ({} bytes)", plaintext.size());
        return {};
    }
    return out;
}

std::optional<Bytes> chachaOpen(const Bytes& key, const Bytes& nonce, const Bytes& ciphertext_with_tag,
                                const Bytes& aad)
{
    if (key.size() != kChachaKeySize || nonce.size() != kChachaNonceSize ||
        ciphertext_with_tag.size() < kChachaTagSize)
    {
        SPDLOG_ERROR("[airplay] chachaOpen: bad sizes (key {} bytes, nonce {} bytes, input {} bytes)", key.size(),
                     nonce.size(), ciphertext_with_tag.size());
        return std::nullopt;
    }

    const size_t body = ciphertext_with_tag.size() - kChachaTagSize;

    CipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(kChachaNonceSize), nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), nonce.data()) != 1)
    {
        SPDLOG_ERROR("[airplay] chachaOpen: init failed (input {} bytes, aad {} bytes)", ciphertext_with_tag.size(),
                     aad.size());
        return std::nullopt;
    }

    int length = 0;
    if (!aad.empty() &&
        EVP_DecryptUpdate(ctx.get(), nullptr, &length, aad.data(), static_cast<int>(aad.size())) != 1)
    {
        SPDLOG_ERROR("[airplay] chachaOpen: aad update failed ({} bytes)", aad.size());
        return std::nullopt;
    }

    Bytes out(body);
    if (body > 0 &&
        EVP_DecryptUpdate(ctx.get(), out.data(), &length, ciphertext_with_tag.data(), static_cast<int>(body)) != 1)
    {
        SPDLOG_ERROR("[airplay] chachaOpen: decrypt failed ({} bytes)", body);
        return std::nullopt;
    }

    // EVP_CTRL_AEAD_SET_TAG wants a mutable pointer even though it only reads.
    Bytes tag(ciphertext_with_tag.begin() + static_cast<ptrdiff_t>(body), ciphertext_with_tag.end());
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_TAG, static_cast<int>(kChachaTagSize), tag.data()) != 1)
    {
        SPDLOG_ERROR("[airplay] chachaOpen: tag install failed");
        return std::nullopt;
    }

    int final_length = 0;
    if (EVP_DecryptFinal_ex(ctx.get(), out.data() + body, &final_length) != 1)
    {
        SPDLOG_ERROR("[airplay] chachaOpen: authentication failed ({} bytes ciphertext, {} bytes aad)", body,
                     aad.size());
        return std::nullopt;
    }
    return out;
}

Bytes nonce64(uint64_t counter)
{
    Bytes nonce(12, 0);
    for (size_t i = 0; i < 8; ++i)
    {
        nonce[4 + i] = static_cast<uint8_t>((counter >> (8 * i)) & 0xff);
    }
    return nonce;
}

Bytes nonceLabel(std::string_view label)
{
    Bytes nonce(12, 0);
    const size_t take = std::min<size_t>(label.size(), 8);
    if (label.size() > 8)
    {
        SPDLOG_ERROR("[airplay] nonceLabel: label too long ({} bytes, truncating to 8)", label.size());
    }
    for (size_t i = 0; i < take; ++i)
    {
        nonce[4 + i] = static_cast<uint8_t>(label[i]);
    }
    return nonce;
}

// --- AES-128-CTR ------------------------------------------------------------

Bytes aesCtr128(const Bytes& key, const Bytes& iv, const Bytes& data)
{
    if (key.size() != kAes128KeySize || iv.size() != kAes128BlockSize)
    {
        SPDLOG_ERROR("[airplay] aesCtr128: bad sizes (key {} bytes, iv {} bytes)", key.size(), iv.size());
        return {};
    }

    CipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_128_ctr(), nullptr, key.data(), iv.data()) != 1)
    {
        SPDLOG_ERROR("[airplay] aesCtr128: init failed (data {} bytes)", data.size());
        return {};
    }

    Bytes out(data.size() + kAes128BlockSize);
    int length = 0;
    if (!data.empty() &&
        EVP_EncryptUpdate(ctx.get(), out.data(), &length, data.data(), static_cast<int>(data.size())) != 1)
    {
        SPDLOG_ERROR("[airplay] aesCtr128: update failed ({} bytes)", data.size());
        return {};
    }

    int final_length = 0;
    if (EVP_EncryptFinal_ex(ctx.get(), out.data() + length, &final_length) != 1)
    {
        SPDLOG_ERROR("[airplay] aesCtr128: finalize failed ({} bytes)", data.size());
        return {};
    }
    out.resize(static_cast<size_t>(length) + static_cast<size_t>(final_length));
    return out;
}

// --- Misc -------------------------------------------------------------------

Bytes randomBytes(size_t length)
{
    Bytes out(length);
    if (length > 0 && RAND_bytes(out.data(), static_cast<int>(length)) != 1)
    {
        SPDLOG_ERROR("[airplay] randomBytes: RAND_bytes failed ({} bytes)", length);
        return {};
    }
    return out;
}

std::string randomId()
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    const Bytes bytes = randomBytes(6);
    if (bytes.size() != 6)
    {
        return {};
    }

    std::string out;
    out.reserve(17);
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        if (i != 0)
        {
            out.push_back(':');
        }
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0x0f]);
    }
    return out;
}

}  // namespace airplay::crypto
