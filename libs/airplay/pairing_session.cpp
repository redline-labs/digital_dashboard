// SPDX-License-Identifier: GPL-3.0-or-later

#include "airplay/pairing_session.h"

#include "airplay/pairing_store.h"
#include "airplay/srp.h"
#include "airplay/tlv8.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <string_view>
#include <utility>

namespace airplay
{
namespace
{

// TLV8 types, from the HomeKit pairing protocol CarPlay reuses.
constexpr uint8_t kTlvIdentifier = 0x01;
constexpr uint8_t kTlvSalt = 0x02;
constexpr uint8_t kTlvPublicKey = 0x03;
constexpr uint8_t kTlvProof = 0x04;
constexpr uint8_t kTlvEncryptedData = 0x05;
constexpr uint8_t kTlvState = 0x06;
constexpr uint8_t kTlvError = 0x07;
constexpr uint8_t kTlvSignature = 0x0A;

constexpr uint8_t kErrorAuthentication = 0x02;

// Pairing bodies are TLV8, but the phone labels them as a plist. We answer with
// the honest type; nothing has ever objected.
constexpr const char* kTlvContentType = "application/octet-stream";

// A pairing failure is a TLV error inside a 200, not an RTSP error status: the
// phone reads the state and retries, where a 4xx aborts the connection.
Bytes tlvError(uint8_t state, uint8_t error)
{
    return tlv8::encode({{kTlvState, {state}}, {kTlvError, {error}}});
}

}  // namespace

// Everything the three handshakes accumulate. Held behind a pointer so the
// header does not have to name the SRP and key types.
struct PairingSession::State
{
    explicit State(std::string state_dir) : store(std::move(state_dir))
    {
        identity = store.loadOrCreateIdentity();
    }

    PairingStore store;

    // True when pair-verify checked the phone against a key already on file.
    bool recognised = false;

    // pair-setup (SRP). Recreated per attempt: the phone retries from M1.
    std::unique_ptr<srp::Server> srp_server;
    Bytes srp_session_key;

    // Long-term accessory identity, used to sign pair-setup M6 and pair-verify.
    // Loaded from the store, or generated and saved on first run.
    crypto::Ed25519Pair identity;

    // The phone's identity, learned in M5.
    std::string device_identifier;
    Bytes device_ltpk;

    // pair-verify ephemeral exchange.
    crypto::X25519Pair verify_ephemeral;
    Bytes device_ephemeral;
    Bytes verify_shared;
    Bytes verify_session_key;

    // MFiSAP (/auth-setup).
    crypto::X25519Pair auth_ephemeral;
    Bytes auth_shared;

    // Session keys for the encrypted control channel that follows pair-verify.
    Bytes control_read;
    Bytes control_write;
    bool verified = false;

    bool paired = false;
};

PairingSession::PairingSession(Config config) :
    state_(std::make_unique<State>(config.state_dir)), config_(std::move(config))
{
    if (state_->store.enabled())
    {
        SPDLOG_INFO("[airplay] pairing store: {} phone(s) known", state_->store.phoneCount());
    }
}

PairingSession::~PairingSession() = default;

bool PairingSession::paired() const
{
    return state_->paired;
}

bool PairingSession::verified() const
{
    return state_->verified;
}

bool PairingSession::recognised() const
{
    return state_->recognised;
}

const Bytes& PairingSession::controlReadKey() const
{
    return state_->control_read;
}

const Bytes& PairingSession::controlWriteKey() const
{
    return state_->control_write;
}

const Bytes& PairingSession::verifySharedSecret() const
{
    return state_->verify_shared;
}

std::string PairingSession::setupPassword()
{
    if (const char* override_value = std::getenv("AIRPLAY_SETUP_PASSWORD");
        override_value != nullptr)
    {
        return override_value;
    }
    return "3939";
}

rtsp::Message PairingSession::handlePairSetup(const rtsp::Message& request)
{
    const auto items = tlv8::decode(request.body);
    const Bytes* state_tlv = tlv8::find(items, kTlvState);
    const uint8_t state = (state_tlv != nullptr && !state_tlv->empty()) ? (*state_tlv)[0] : 0;

    SPDLOG_INFO("[airplay] pair-setup M{}", state);

    switch (state)
    {
        case 1:
        {
            // M1 -> M2. Stand up a fresh SRP server; the phone always restarts
            // the exchange from M1, so any half-finished attempt is discarded.
            state_->srp_server =
                std::make_unique<srp::Server>(srp::kPairSetupUsername, setupPassword());
            if (!state_->srp_server->valid())
            {
                SPDLOG_ERROR("[airplay] could not initialise SRP");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(2, kErrorAuthentication));
            }

            const Bytes& salt = state_->srp_server->salt();
            const Bytes& public_b = state_->srp_server->publicB();
            SPDLOG_INFO("[airplay] pair-setup M2: salt {} bytes, B {} bytes", salt.size(),
                        public_b.size());

            return rtsp::makeResponse(200, "OK", kTlvContentType,
                                      tlv8::encode({{kTlvState, {2}},
                                                    {kTlvPublicKey, public_b},
                                                    {kTlvSalt, salt}}));
        }

        case 3:
        {
            // M3 -> M4. The phone proves it knows the password; if our password
            // guess is wrong this is exactly where it shows up.
            if (!state_->srp_server)
            {
                SPDLOG_ERROR("[airplay] pair-setup M3 with no M1 in progress");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(4, kErrorAuthentication));
            }
            const Bytes* client_a = tlv8::find(items, kTlvPublicKey);
            const Bytes* client_m1 = tlv8::find(items, kTlvProof);
            if (client_a == nullptr || client_m1 == nullptr)
            {
                SPDLOG_ERROR("[airplay] pair-setup M3 missing PublicKey or Proof");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(4, kErrorAuthentication));
            }

            const auto result = state_->srp_server->verify(*client_a, *client_m1);
            if (!result.ok)
            {
                SPDLOG_ERROR("[airplay] pair-setup M3 proof REJECTED -- the setup password is "
                             "wrong (currently '{}'). Override with AIRPLAY_SETUP_PASSWORD.",
                             setupPassword());
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(4, kErrorAuthentication));
            }

            state_->srp_session_key = result.session_key;
            SPDLOG_INFO("[airplay] pair-setup M4: proof ACCEPTED, session key {} bytes",
                        result.session_key.size());
            return rtsp::makeResponse(
                200, "OK", kTlvContentType,
                tlv8::encode({{kTlvState, {4}}, {kTlvProof, result.server_proof}}));
        }

        case 5:
        {
            // M5 -> M6: both sides hand over their long-term Ed25519 identity,
            // encrypted under a key derived from the SRP session key.
            if (state_->srp_session_key.empty())
            {
                SPDLOG_ERROR("[airplay] pair-setup M5 with no session key");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(6, kErrorAuthentication));
            }
            const Bytes* encrypted = tlv8::find(items, kTlvEncryptedData);
            if (encrypted == nullptr)
            {
                SPDLOG_ERROR("[airplay] pair-setup M5 missing EncryptedData");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(6, kErrorAuthentication));
            }

            const Bytes session_key = crypto::hkdfSha512(
                state_->srp_session_key, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", 32);

            const auto plain =
                crypto::chachaOpen(session_key, crypto::nonceLabel("PS-Msg05"), *encrypted);
            if (!plain)
            {
                SPDLOG_ERROR("[airplay] pair-setup M5 decryption failed");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(6, kErrorAuthentication));
            }

            const auto inner = tlv8::decode(*plain);
            const Bytes* device_id = tlv8::find(inner, kTlvIdentifier);
            const Bytes* device_ltpk = tlv8::find(inner, kTlvPublicKey);
            SPDLOG_INFO("[airplay] pair-setup M5: device id {} bytes, LTPK {} bytes",
                        device_id != nullptr ? device_id->size() : 0,
                        device_ltpk != nullptr ? device_ltpk->size() : 0);
            if (device_id != nullptr)
            {
                state_->device_identifier.assign(device_id->begin(), device_id->end());
            }
            if (device_ltpk != nullptr)
            {
                state_->device_ltpk = *device_ltpk;
                // Persist it so this phone is recognised on the next run
                // instead of pairing from scratch.
                state_->store.savePhoneKey(state_->device_identifier, *device_ltpk);
            }

            // M6: our identifier, our long-term public key, and a signature
            // over (accessory-x | identifier | LTPK) proving we hold the key.
            const Bytes accessory_x = crypto::hkdfSha512(state_->srp_session_key,
                                                         "Pair-Setup-Accessory-Sign-Salt",
                                                         "Pair-Setup-Accessory-Sign-Info", 32);
            const Bytes identifier(config_.identifier.begin(), config_.identifier.end());

            Bytes to_sign = accessory_x;
            to_sign.insert(to_sign.end(), identifier.begin(), identifier.end());
            to_sign.insert(to_sign.end(), state_->identity.public_key.begin(),
                           state_->identity.public_key.end());
            const Bytes signature = crypto::ed25519Sign(state_->identity.private_key, to_sign);

            const Bytes sub = tlv8::encode({{kTlvIdentifier, identifier},
                                            {kTlvPublicKey, state_->identity.public_key},
                                            {kTlvSignature, signature}});
            const Bytes sealed =
                crypto::chachaSeal(session_key, crypto::nonceLabel("PS-Msg06"), sub);

            state_->paired = true;
            SPDLOG_INFO("[airplay] pair-setup M6: sending accessory identity, PAIRED");
            return rtsp::makeResponse(
                200, "OK", kTlvContentType,
                tlv8::encode({{kTlvState, {6}}, {kTlvEncryptedData, sealed}}));
        }

        default:
            SPDLOG_WARN("[airplay] unexpected pair-setup state {}", state);
            return rtsp::makeResponse(200, "OK", kTlvContentType,
                                      tlvError(static_cast<uint8_t>(state + 1),
                                               kErrorAuthentication));
    }
}

rtsp::Message PairingSession::handlePairVerify(const rtsp::Message& request)
{
    const auto items = tlv8::decode(request.body);
    const Bytes* state_tlv = tlv8::find(items, kTlvState);
    const uint8_t state = (state_tlv != nullptr && !state_tlv->empty()) ? (*state_tlv)[0] : 0;

    SPDLOG_INFO("[airplay] pair-verify M{}", state);

    switch (state)
    {
        case 1:
        {
            // M1 -> M2. Ephemeral X25519 exchange, then prove we hold the
            // long-term key the phone learned during pair-setup.
            const Bytes* device_public = tlv8::find(items, kTlvPublicKey);
            if (device_public == nullptr || device_public->size() != 32)
            {
                SPDLOG_ERROR("[airplay] pair-verify M1 missing or malformed PublicKey");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(2, kErrorAuthentication));
            }

            state_->verify_ephemeral = crypto::x25519Generate();
            state_->device_ephemeral = *device_public;
            state_->verify_shared =
                crypto::x25519Shared(state_->verify_ephemeral.private_key, *device_public);
            if (state_->verify_shared.empty())
            {
                SPDLOG_ERROR("[airplay] pair-verify X25519 exchange failed");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(2, kErrorAuthentication));
            }

            const Bytes session_key =
                crypto::hkdfSha512(state_->verify_shared, "Pair-Verify-Encrypt-Salt",
                                   "Pair-Verify-Encrypt-Info", 32);
            state_->verify_session_key = session_key;

            const Bytes identifier(config_.identifier.begin(), config_.identifier.end());

            // Signed material is our ephemeral public key, our identifier, then
            // the phone's ephemeral public key -- in that order.
            Bytes to_sign = state_->verify_ephemeral.public_key;
            to_sign.insert(to_sign.end(), identifier.begin(), identifier.end());
            to_sign.insert(to_sign.end(), device_public->begin(), device_public->end());
            const Bytes signature = crypto::ed25519Sign(state_->identity.private_key, to_sign);

            const Bytes sub =
                tlv8::encode({{kTlvIdentifier, identifier}, {kTlvSignature, signature}});
            const Bytes sealed =
                crypto::chachaSeal(session_key, crypto::nonceLabel("PV-Msg02"), sub);

            SPDLOG_INFO("[airplay] pair-verify M2: ephemeral key exchanged, signing identity");
            return rtsp::makeResponse(200, "OK", kTlvContentType,
                                      tlv8::encode({{kTlvState, {2}},
                                                    {kTlvPublicKey,
                                                     state_->verify_ephemeral.public_key},
                                                    {kTlvEncryptedData, sealed}}));
        }

        case 3:
        {
            // M3 -> M4. The phone proves it holds the key it gave us in
            // pair-setup M5.
            if (state_->verify_session_key.empty())
            {
                SPDLOG_ERROR("[airplay] pair-verify M3 with no M1 in progress");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(4, kErrorAuthentication));
            }
            const Bytes* encrypted = tlv8::find(items, kTlvEncryptedData);
            if (encrypted == nullptr)
            {
                SPDLOG_ERROR("[airplay] pair-verify M3 missing EncryptedData");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(4, kErrorAuthentication));
            }

            const auto plain = crypto::chachaOpen(state_->verify_session_key,
                                                  crypto::nonceLabel("PV-Msg03"), *encrypted);
            if (!plain)
            {
                SPDLOG_ERROR("[airplay] pair-verify M3 decryption failed");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(4, kErrorAuthentication));
            }

            const auto inner = tlv8::decode(*plain);
            const Bytes* identifier = tlv8::find(inner, kTlvIdentifier);
            const Bytes* signature = tlv8::find(inner, kTlvSignature);
            if (identifier == nullptr || signature == nullptr)
            {
                SPDLOG_ERROR("[airplay] pair-verify M3 inner TLV incomplete");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(4, kErrorAuthentication));
            }

            // Mirror image of what we signed in M2.
            Bytes signed_material = state_->device_ephemeral;
            signed_material.insert(signed_material.end(), identifier->begin(), identifier->end());
            signed_material.insert(signed_material.end(),
                                   state_->verify_ephemeral.public_key.begin(),
                                   state_->verify_ephemeral.public_key.end());

            // Prefer the key on file over the one this session's pair-setup
            // handed us. They are normally the same; when they differ, the
            // stored one is the one that means something -- it is evidence
            // about the phone from before this connection existed.
            const std::string phone_id(identifier->begin(), identifier->end());
            const auto stored = state_->store.phoneKey(phone_id);
            const Bytes& expected = stored ? *stored : state_->device_ltpk;
            state_->recognised = stored.has_value();

            if (expected.empty())
            {
                // No pair-setup this session and nothing on file. Nothing to
                // check the signature against, so there is no security here to
                // claim -- say so rather than logging a reassuring "verified".
                SPDLOG_WARN("[airplay] pair-verify M3 from '{}' with no key to check it against "
                            "(no pair-setup this session, nothing on file); allowing",
                            phone_id);
            }
            else if (!crypto::ed25519Verify(expected, signed_material, *signature))
            {
                // With a key on file this is a real failure: either the phone
                // is not who it claims, or it rotated its key without redoing
                // pair-setup. Refuse -- the whole point of persisting the key
                // was to be able to.
                SPDLOG_ERROR("[airplay] pair-verify M3 signature from '{}' did NOT verify against "
                             "the {} key; refusing",
                             phone_id, stored ? "stored" : "session");
                return rtsp::makeResponse(200, "OK", kTlvContentType,
                                          tlvError(4, kErrorAuthentication));
            }
            else
            {
                SPDLOG_INFO("[airplay] pair-verify M3 signature verified against the {} key",
                            stored ? "stored" : "session");
            }

            // Control channel keys for everything after this point.
            state_->control_read = crypto::hkdfSha512(state_->verify_shared, "Control-Salt",
                                                      "Control-Read-Encryption-Key", 32);
            state_->control_write = crypto::hkdfSha512(state_->verify_shared, "Control-Salt",
                                                       "Control-Write-Encryption-Key", 32);
            state_->verified = true;

            SPDLOG_INFO("[airplay] pair-verify M4: VERIFIED, control channel keys derived");
            return rtsp::makeResponse(200, "OK", kTlvContentType,
                                      tlv8::encode({{kTlvState, {4}}}));
        }

        default:
            SPDLOG_WARN("[airplay] unexpected pair-verify state {}", state);
            return rtsp::makeResponse(200, "OK", kTlvContentType,
                                      tlvError(static_cast<uint8_t>(state + 1),
                                               kErrorAuthentication));
    }
}

rtsp::Message PairingSession::handleAuthSetup(const rtsp::Message& request)
{
    // MFiSAP: one byte of mode followed by the phone's Curve25519 public key.
    if (request.body.size() != 33)
    {
        SPDLOG_ERROR("[airplay] auth-setup body is {} bytes, expected 33", request.body.size());
        return rtsp::makeResponse(400, "Bad Request", "", {});
    }
    if (!config_.mfi_certificate || !config_.mfi_sign)
    {
        SPDLOG_ERROR("[airplay] auth-setup needs the MFi coprocessor and none is wired up");
        return rtsp::makeResponse(501, "Not Implemented", "", {});
    }

    const uint8_t mode = request.body[0];
    const Bytes device_public(request.body.begin() + 1, request.body.end());
    SPDLOG_INFO("[airplay] auth-setup mode {}, device key {} bytes", mode, device_public.size());

    // Our ephemeral key, and the shared secret the media streams are keyed from.
    state_->auth_ephemeral = crypto::x25519Generate();
    state_->auth_shared =
        crypto::x25519Shared(state_->auth_ephemeral.private_key, device_public);
    if (state_->auth_shared.empty())
    {
        SPDLOG_ERROR("[airplay] auth-setup X25519 exchange failed");
        return rtsp::makeResponse(500, "Internal Server Error", "", {});
    }

    const Bytes certificate = config_.mfi_certificate();
    if (certificate.empty())
    {
        SPDLOG_ERROR("[airplay] auth-setup: coprocessor returned no certificate");
        return rtsp::makeResponse(500, "Internal Server Error", "", {});
    }

    // The coprocessor signs a digest of our public key followed by theirs. The
    // digest width follows the authentication protocol major version: 2 uses
    // SHA-1, 3 uses SHA-256. Signing the wrong width fails quietly, so this is
    // driven by what the chip reports rather than assumed.
    const int major = config_.mfi_protocol_major ? config_.mfi_protocol_major() : 2;

    // The signed message is our public key followed by theirs.
    const std::vector<Bytes> operands{state_->auth_ephemeral.public_key, device_public};
    const Bytes digest = (major >= 3) ? crypto::sha256(operands) : crypto::sha1(operands);
    SPDLOG_DEBUG("[airplay] auth-setup signing a {}-byte digest (protocol major {})",
                 digest.size(), major);

    const Bytes signature = config_.mfi_sign(digest);
    if (signature.empty())
    {
        SPDLOG_ERROR("[airplay] auth-setup: coprocessor did not sign the challenge");
        return rtsp::makeResponse(500, "Internal Server Error", "", {});
    }

    // The signature travels encrypted under AES-128-CTR -- this is what
    // crypto.h's aesCtr128 exists for. Key and IV are SHA-1 (not SHA-512) of a
    // short ASCII label prepended to the shared secret, truncated to 16 bytes.
    // The certificate is public and goes in the clear.
    const auto label = [](std::string_view text) {
        return Bytes(text.begin(), text.end());
    };
    const Bytes key_material = crypto::sha1({label("AES-KEY"), state_->auth_shared});
    const Bytes iv_material = crypto::sha1({label("AES-IV"), state_->auth_shared});
    if (key_material.size() < 16 || iv_material.size() < 16)
    {
        SPDLOG_ERROR("[airplay] auth-setup: AES key derivation failed");
        return rtsp::makeResponse(500, "Internal Server Error", "", {});
    }
    const Bytes aes_key(key_material.begin(), key_material.begin() + 16);
    const Bytes aes_iv(iv_material.begin(), iv_material.begin() + 16);

    const Bytes sealed_signature = crypto::aesCtr128(aes_key, aes_iv, signature);
    if (sealed_signature.empty())
    {
        SPDLOG_ERROR("[airplay] auth-setup: AES-CTR of the signature failed");
        return rtsp::makeResponse(500, "Internal Server Error", "", {});
    }

    // Layout: our public key, then the certificate and the encrypted signature,
    // each preceded by a big-endian 32-bit length.
    Bytes body = state_->auth_ephemeral.public_key;
    const auto append_be32 = [&body](size_t value) {
        body.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        body.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        body.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        body.push_back(static_cast<uint8_t>(value & 0xFF));
    };
    append_be32(certificate.size());
    body.insert(body.end(), certificate.begin(), certificate.end());
    append_be32(sealed_signature.size());
    body.insert(body.end(), sealed_signature.begin(), sealed_signature.end());

    SPDLOG_INFO("[airplay] auth-setup: signature {} bytes, certificate {} bytes, replying {}",
                signature.size(), certificate.size(), body.size());
    return rtsp::makeResponse(200, "OK", "application/octet-stream", std::move(body));
}
}  // namespace airplay
