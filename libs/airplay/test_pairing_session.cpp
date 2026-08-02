// SPDX-License-Identifier: GPL-3.0-or-later
//
// The three handshakes, driven from the phone's side.
//
// Every failure in here is silent on the wire: a phone that dislikes what it
// gets simply stops, with no error and nothing in a log to say which of the six
// messages was wrong. So the point of these tests is to run the exchange
// end to end against a stand-in phone -- a real SRP client, real X25519, real
// Ed25519 -- and check the accessory's half at each step.
//
// What they cannot check is whether Apple's phone agrees with our reading of
// the protocol. They pin the reading down so it stops moving.
#include "airplay/pairing_session.h"

#include "airplay/crypto.h"
#include "airplay/srp.h"
#include "airplay/tlv8.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <optional>
#include <string>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

using airplay::Bytes;
namespace tlv8 = airplay::tlv8;
namespace crypto = airplay::crypto;

constexpr uint8_t kTlvIdentifier = 0x01;
constexpr uint8_t kTlvSalt = 0x02;
constexpr uint8_t kTlvPublicKey = 0x03;
constexpr uint8_t kTlvProof = 0x04;
constexpr uint8_t kTlvEncryptedData = 0x05;
constexpr uint8_t kTlvState = 0x06;
constexpr uint8_t kTlvError = 0x07;
constexpr uint8_t kTlvSignature = 0x0A;

constexpr const char* kIdentifier = "Test Dashboard";

airplay::rtsp::Message request(std::string uri, Bytes body)
{
    airplay::rtsp::Message message;
    message.method = "POST";
    message.uri = std::move(uri);
    message.body = std::move(body);
    return message;
}

// The TLV state byte in a reply, or 0 if absent.
uint8_t stateOf(const std::vector<tlv8::Item>& items)
{
    const Bytes* value = tlv8::find(items, kTlvState);
    return (value != nullptr && !value->empty()) ? (*value)[0] : 0;
}

bool isError(const std::vector<tlv8::Item>& items)
{
    return tlv8::find(items, kTlvError) != nullptr;
}

// A pairing failure must be a TLV error inside a 200: an RTSP error status
// aborts the connection instead of letting the phone retry.
void expectTlvError(const airplay::rtsp::Message& reply, uint8_t expected_state,
                    const std::string& what)
{
    expect(reply.status == 200, what + " answers 200, not an RTSP error");
    const auto items = tlv8::decode(reply.body);
    expect(isError(items), what + " carries a TLV error");
    expect(stateOf(items) == expected_state, what + " reports the paired state");
}

// What a completed pair-setup leaves the stand-in phone holding.
struct PairedPhone
{
    Bytes accessory_ltpk;
    crypto::Ed25519Pair identity;
};

// Runs pair-setup M1..M6 against `session` as the phone would.
std::optional<PairedPhone> runPairSetup(airplay::PairingSession& session)
{
    // M1 -> M2
    const auto m2 = session.handlePairSetup(
        request("/pair-setup", tlv8::encode({{kTlvState, {1}}, {0x00, {0}}})));
    const auto m2_items = tlv8::decode(m2.body);
    if (stateOf(m2_items) != 2 || isError(m2_items))
    {
        expect(false, "pair-setup M2 was not produced");
        return std::nullopt;
    }
    const Bytes* salt = tlv8::find(m2_items, kTlvSalt);
    const Bytes* server_b = tlv8::find(m2_items, kTlvPublicKey);
    expect(salt != nullptr && salt->size() == 16, "M2 carries a 16-byte salt");
    expect(server_b != nullptr && !server_b->empty(), "M2 carries the server's public B");
    if (salt == nullptr || server_b == nullptr)
    {
        return std::nullopt;
    }

    // M3 -> M4. The password is the one the accessory uses; getting it wrong is
    // exactly what the negative test below covers.
    airplay::srp::Client client(airplay::srp::kPairSetupUsername,
                                airplay::PairingSession::setupPassword());
    const auto proof = client.computeProof(*salt, *server_b);
    expect(proof.ok, "the stand-in phone computes its proof");

    const auto m4 = session.handlePairSetup(request(
        "/pair-setup",
        tlv8::encode({{kTlvState, {3}}, {kTlvPublicKey, client.publicA()}, {kTlvProof, proof.client_proof}})));
    const auto m4_items = tlv8::decode(m4.body);
    expect(!isError(m4_items) && stateOf(m4_items) == 4, "the accessory accepts a correct proof");
    if (isError(m4_items))
    {
        return std::nullopt;
    }
    const Bytes* server_proof = tlv8::find(m4_items, kTlvProof);
    expect(server_proof != nullptr && client.checkServerProof(*server_proof),
           "and proves itself back with M2");

    // M5 -> M6. Both sides hand over their long-term identity, encrypted under
    // a key derived from the SRP session key.
    const Bytes encrypt_key = crypto::hkdfSha512(proof.session_key, "Pair-Setup-Encrypt-Salt",
                                                 "Pair-Setup-Encrypt-Info", 32);
    PairedPhone phone;
    phone.identity = crypto::ed25519Generate();
    const Bytes phone_id{'p', 'h', 'o', 'n', 'e'};
    const Bytes inner = tlv8::encode(
        {{kTlvIdentifier, phone_id}, {kTlvPublicKey, phone.identity.public_key}});
    const Bytes sealed = crypto::chachaSeal(encrypt_key, crypto::nonceLabel("PS-Msg05"), inner);

    const auto m6 = session.handlePairSetup(
        request("/pair-setup", tlv8::encode({{kTlvState, {5}}, {kTlvEncryptedData, sealed}})));
    const auto m6_items = tlv8::decode(m6.body);
    expect(!isError(m6_items) && stateOf(m6_items) == 6, "the accessory completes pair-setup");
    if (isError(m6_items))
    {
        return std::nullopt;
    }

    const Bytes* m6_sealed = tlv8::find(m6_items, kTlvEncryptedData);
    expect(m6_sealed != nullptr, "M6 carries encrypted data");
    if (m6_sealed == nullptr)
    {
        return std::nullopt;
    }
    const auto m6_plain =
        crypto::chachaOpen(encrypt_key, crypto::nonceLabel("PS-Msg06"), *m6_sealed);
    expect(m6_plain.has_value(), "M6 decrypts under the derived key");
    if (!m6_plain)
    {
        return std::nullopt;
    }

    const auto m6_inner = tlv8::decode(*m6_plain);
    const Bytes* identifier = tlv8::find(m6_inner, kTlvIdentifier);
    const Bytes* ltpk = tlv8::find(m6_inner, kTlvPublicKey);
    const Bytes* signature = tlv8::find(m6_inner, kTlvSignature);
    expect(identifier != nullptr &&
               std::string(identifier->begin(), identifier->end()) == kIdentifier,
           "M6 identifies the accessory by the name /info advertises");
    expect(ltpk != nullptr && ltpk->size() == 32, "M6 carries the accessory's long-term key");
    expect(signature != nullptr, "M6 carries a signature");

    // The signature is over accessory-x | identifier | LTPK. Checking it is what
    // proves the accessory holds the key it just handed over.
    if (ltpk != nullptr && signature != nullptr && identifier != nullptr)
    {
        const Bytes accessory_x = crypto::hkdfSha512(
            proof.session_key, "Pair-Setup-Accessory-Sign-Salt", "Pair-Setup-Accessory-Sign-Info", 32);
        Bytes signed_material = accessory_x;
        signed_material.insert(signed_material.end(), identifier->begin(), identifier->end());
        signed_material.insert(signed_material.end(), ltpk->begin(), ltpk->end());
        expect(crypto::ed25519Verify(*ltpk, signed_material, *signature),
               "M6's signature verifies against the key it carries");
        phone.accessory_ltpk = *ltpk;
    }
    return phone;
}

airplay::PairingSession::Config makeConfig()
{
    airplay::PairingSession::Config config;
    config.identifier = kIdentifier;
    return config;
}

}  // namespace

int main()
{
    using airplay::PairingSession;

    // A full pair-setup, then pair-verify on top of it.
    {
        PairingSession session(makeConfig());
        expect(!session.paired(), "a fresh session is not paired");
        expect(!session.verified(), "nor verified");

        const auto phone = runPairSetup(session);
        expect(phone.has_value(), "pair-setup runs to completion");
        expect(session.paired(), "and the session reports itself paired");
        expect(!session.verified(), "but not yet verified -- that is pair-verify's job");

        if (phone)
        {
            // pair-verify M1 -> M2.
            const crypto::X25519Pair ephemeral = crypto::x25519Generate();
            const auto m2 = session.handlePairVerify(request(
                "/pair-verify",
                tlv8::encode({{kTlvState, {1}}, {kTlvPublicKey, ephemeral.public_key}})));
            const auto m2_items = tlv8::decode(m2.body);
            expect(!isError(m2_items) && stateOf(m2_items) == 2, "pair-verify M2 is produced");

            const Bytes* accessory_ephemeral = tlv8::find(m2_items, kTlvPublicKey);
            const Bytes* sealed = tlv8::find(m2_items, kTlvEncryptedData);
            expect(accessory_ephemeral != nullptr && accessory_ephemeral->size() == 32,
                   "M2 carries the accessory's ephemeral key");

            if (accessory_ephemeral != nullptr && sealed != nullptr)
            {
                const Bytes shared =
                    crypto::x25519Shared(ephemeral.private_key, *accessory_ephemeral);
                expect(!shared.empty(), "both sides reach the same X25519 secret");

                const Bytes session_key = crypto::hkdfSha512(shared, "Pair-Verify-Encrypt-Salt",
                                                             "Pair-Verify-Encrypt-Info", 32);
                const auto plain =
                    crypto::chachaOpen(session_key, crypto::nonceLabel("PV-Msg02"), *sealed);
                expect(plain.has_value(), "M2 decrypts under the derived key");

                if (plain)
                {
                    const auto inner = tlv8::decode(*plain);
                    const Bytes* identifier = tlv8::find(inner, kTlvIdentifier);
                    const Bytes* signature = tlv8::find(inner, kTlvSignature);
                    expect(identifier != nullptr && signature != nullptr, "M2 inner TLV is complete");

                    // Signed material is accessory ephemeral | identifier |
                    // phone ephemeral, in that order. The order is the part
                    // that is easy to get wrong and impossible to observe.
                    if (identifier != nullptr && signature != nullptr)
                    {
                        Bytes signed_material = *accessory_ephemeral;
                        signed_material.insert(signed_material.end(), identifier->begin(),
                                               identifier->end());
                        signed_material.insert(signed_material.end(), ephemeral.public_key.begin(),
                                               ephemeral.public_key.end());
                        expect(crypto::ed25519Verify(phone->accessory_ltpk, signed_material,
                                                     *signature),
                               "M2's signature verifies against the LTPK from pair-setup");
                    }
                }

                // M3 -> M4: the phone proves it holds the key it gave in M5.
                //
                // The signed material is the mirror image of M2's: the phone's
                // ephemeral key, the phone's identifier, then the accessory's
                // ephemeral key. Note the accessory currently only *warns* when
                // this does not verify (it holds no pair record across runs, so
                // the LTPK it has is this session's), which means this exchange
                // completing does not on its own prove the check works. What it
                // does pin down is the layout, so the check starts out right
                // when the pair record lands.
                const Bytes phone_id{'p', 'h', 'o', 'n', 'e'};
                Bytes material = ephemeral.public_key;
                material.insert(material.end(), phone_id.begin(), phone_id.end());
                material.insert(material.end(), accessory_ephemeral->begin(),
                                accessory_ephemeral->end());

                const Bytes phone_signature =
                    crypto::ed25519Sign(phone->identity.private_key, material);
                const Bytes phone_inner = tlv8::encode(
                    {{kTlvIdentifier, phone_id}, {kTlvSignature, phone_signature}});
                const Bytes phone_sealed =
                    crypto::chachaSeal(session_key, crypto::nonceLabel("PV-Msg03"), phone_inner);

                const auto m4 = session.handlePairVerify(request(
                    "/pair-verify",
                    tlv8::encode({{kTlvState, {3}}, {kTlvEncryptedData, phone_sealed}})));
                const auto m4_items = tlv8::decode(m4.body);
                expect(!isError(m4_items) && stateOf(m4_items) == 4, "pair-verify completes");
                expect(session.verified(), "and the session reports itself verified");

                // The control-channel keys, which everything after this point
                // depends on. Both must exist, differ, and be derivable by the
                // phone from the same shared secret.
                expect(session.controlReadKey().size() == 32, "a control read key is derived");
                expect(session.controlWriteKey().size() == 32, "and a control write key");
                expect(session.controlReadKey() != session.controlWriteKey(),
                       "the two directions do not share a key");
                expect(session.verifySharedSecret() == shared,
                       "the shared secret matches the phone's, so event keys will agree");
                expect(session.controlReadKey() ==
                           crypto::hkdfSha512(shared, "Control-Salt", "Control-Read-Encryption-Key", 32),
                       "the read key is what the phone derives with the same label");
            }
        }
    }

    // A wrong setup password is rejected at M3 -- the single most likely
    // protocol mistake, and the one with a real diagnostic behind it.
    {
        PairingSession session(makeConfig());
        const auto m2 = session.handlePairSetup(
            request("/pair-setup", tlv8::encode({{kTlvState, {1}}})));
        const auto m2_items = tlv8::decode(m2.body);
        const Bytes* salt = tlv8::find(m2_items, kTlvSalt);
        const Bytes* server_b = tlv8::find(m2_items, kTlvPublicKey);

        airplay::srp::Client wrong(airplay::srp::kPairSetupUsername, "0000");
        const auto proof = wrong.computeProof(*salt, *server_b);
        const auto m4 = session.handlePairSetup(request(
            "/pair-setup", tlv8::encode({{kTlvState, {3}},
                                         {kTlvPublicKey, wrong.publicA()},
                                         {kTlvProof, proof.client_proof}})));
        expectTlvError(m4, 4, "a wrong password");
        expect(!session.paired(), "and leaves the session unpaired");
    }

    // Messages out of order, which is how a half-finished attempt shows up.
    {
        PairingSession session(makeConfig());
        expectTlvError(session.handlePairSetup(request("/pair-setup",
                                                       tlv8::encode({{kTlvState, {3}}}))),
                       4, "pair-setup M3 with no M1");
        expectTlvError(session.handlePairSetup(request("/pair-setup",
                                                       tlv8::encode({{kTlvState, {5}}}))),
                       6, "pair-setup M5 with no session key");
        expectTlvError(session.handlePairVerify(request("/pair-verify",
                                                        tlv8::encode({{kTlvState, {3}}}))),
                       4, "pair-verify M3 with no M1");
        expectTlvError(session.handlePairVerify(request("/pair-verify",
                                                        tlv8::encode({{kTlvState, {1}}}))),
                       2, "pair-verify M1 with no public key");

        // The phone restarts from M1 after a failure, so a second M1 has to
        // work rather than being rejected as a duplicate.
        const auto retry = session.handlePairSetup(
            request("/pair-setup", tlv8::encode({{kTlvState, {1}}})));
        expect(!isError(tlv8::decode(retry.body)), "a restarted pair-setup is accepted");
    }

    // auth-setup, which is what proves an Apple coprocessor is present.
    {
        // A stand-in coprocessor: a fixed certificate, and a "signature" that is
        // just the digest reversed. Enough to check the reply's layout and the
        // AES-CTR wrapping, which is all that is ours to get right.
        const Bytes certificate(120, 0xC1);
        PairingSession::Config config = makeConfig();
        config.mfi_certificate = [certificate]() { return certificate; };
        config.mfi_sign = [](const Bytes& digest) { return Bytes(digest.rbegin(), digest.rend()); };
        config.mfi_protocol_major = []() { return 2; };
        PairingSession session(config);

        const crypto::X25519Pair phone = crypto::x25519Generate();
        Bytes body{0x01};
        body.insert(body.end(), phone.public_key.begin(), phone.public_key.end());

        const auto reply = session.handleAuthSetup(request("/auth-setup", body));
        expect(reply.status == 200, "auth-setup succeeds with a coprocessor present");

        // Layout: our 32-byte public key, then the certificate and the
        // encrypted signature, each behind a big-endian 32-bit length.
        expect(reply.body.size() > 32 + 4, "the reply carries a key and at least one blob");
        if (reply.body.size() > 40)
        {
            const Bytes accessory_public(reply.body.begin(), reply.body.begin() + 32);
            const auto be32 = [&reply](size_t offset) {
                return (static_cast<size_t>(reply.body[offset]) << 24) |
                       (static_cast<size_t>(reply.body[offset + 1]) << 16) |
                       (static_cast<size_t>(reply.body[offset + 2]) << 8) |
                       static_cast<size_t>(reply.body[offset + 3]);
            };
            const size_t cert_len = be32(32);
            expect(cert_len == certificate.size(), "the certificate length is big endian");

            const Bytes sent_cert(reply.body.begin() + 36,
                                  reply.body.begin() + 36 + static_cast<long>(cert_len));
            expect(sent_cert == certificate, "the certificate is sent in the clear");

            const size_t sig_offset = 36 + cert_len;
            const size_t sig_len = be32(sig_offset);
            expect(reply.body.size() == sig_offset + 4 + sig_len, "the reply ends after the signature");

            // The signature is AES-128-CTR under keys derived from the shared
            // secret with SHA-1, not SHA-512 -- an easy thing to get wrong, and
            // silent when wrong.
            const Bytes shared = crypto::x25519Shared(phone.private_key, accessory_public);
            const auto label = [](std::string_view text) { return Bytes(text.begin(), text.end()); };
            const Bytes key_material = crypto::sha1({label("AES-KEY"), shared});
            const Bytes iv_material = crypto::sha1({label("AES-IV"), shared});
            const Bytes aes_key(key_material.begin(), key_material.begin() + 16);
            const Bytes aes_iv(iv_material.begin(), iv_material.begin() + 16);

            const Bytes sealed(reply.body.begin() + static_cast<long>(sig_offset) + 4,
                               reply.body.end());
            const Bytes signature = crypto::aesCtr128(aes_key, aes_iv, sealed);

            // Protocol major 2 means a SHA-1 digest, and our stand-in reversed
            // it. Recomputing it here checks the digest width and the operand
            // order at once.
            const Bytes digest = crypto::sha1({accessory_public, phone.public_key});
            expect(signature == Bytes(digest.rbegin(), digest.rend()),
                   "the signature decrypts to what the coprocessor signed");
            expect(digest.size() == 20, "protocol major 2 signs a SHA-1 digest");
        }
    }

    // Protocol major 3 moves to SHA-256. Signing the wrong width fails quietly
    // on hardware, so it is driven by what the chip reports.
    {
        PairingSession::Config config = makeConfig();
        size_t digest_size = 0;
        config.mfi_certificate = []() { return Bytes(10, 0xAB); };
        config.mfi_sign = [&digest_size](const Bytes& digest) {
            digest_size = digest.size();
            return Bytes(8, 0x5A);
        };
        config.mfi_protocol_major = []() { return 3; };
        PairingSession session(config);

        Bytes body{0x01};
        const crypto::X25519Pair phone = crypto::x25519Generate();
        body.insert(body.end(), phone.public_key.begin(), phone.public_key.end());
        session.handleAuthSetup(request("/auth-setup", body));
        expect(digest_size == 32, "protocol major 3 signs a SHA-256 digest");
    }

    // Without a coprocessor the session cannot proceed, and says so rather than
    // pretending to succeed.
    {
        PairingSession session(makeConfig());
        Bytes body(33, 0);
        const auto reply = session.handleAuthSetup(request("/auth-setup", body));
        expect(reply.status == 501, "auth-setup with no coprocessor answers 501");

        const auto short_reply = session.handleAuthSetup(request("/auth-setup", Bytes(10, 0)));
        expect(short_reply.status == 400, "a malformed auth-setup body answers 400");
    }

    if (failures == 0)
    {
        SPDLOG_INFO("pairing session tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
