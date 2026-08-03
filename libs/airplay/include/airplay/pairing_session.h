// SPDX-License-Identifier: GPL-3.0-or-later
//
// The three handshakes a phone runs before it will start a CarPlay session, and
// the accessory identity they establish.
//
//   /pair-setup   SRP over a transient password, ending with both sides holding
//                 the other's long-term Ed25519 public key.
//   /pair-verify  an ephemeral X25519 exchange, each side signing with the
//                 long-term key from pair-setup. Produces the shared secret the
//                 encrypted channels are keyed from.
//   /auth-setup   MFiSAP: proves there is a genuine Apple authentication
//                 coprocessor present. CarPlay does not proceed without it.
//
// Kept apart from the RTSP server because it is a state machine over byte
// strings, with no sockets and no threads in it. Every failure here is silent
// on the wire -- the phone simply stops -- so being able to drive it from a
// test is the only way to tell a protocol mistake from a hardware one.
//
// Not thread safe: one connection drives the whole handshake in order.
#ifndef AIRPLAY_PAIRING_SESSION_H_
#define AIRPLAY_PAIRING_SESSION_H_

#include "airplay/crypto.h"
#include "airplay/rtsp.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace airplay
{

using Bytes = std::vector<uint8_t>;

namespace srp
{
class Server;
}

class PairingSession
{
  public:
    struct Config
    {
        // Advertised as the accessory's identifier, and signed over in both
        // handshakes. Must match the `name` in GET /info.
        std::string identifier = "Dashboard";

        // MFi coprocessor access for /auth-setup. Left empty, auth-setup answers
        // 501, which stops the session: CarPlay will not proceed without a
        // genuine Apple authentication chip.
        std::function<Bytes()> mfi_certificate;
        std::function<Bytes(const Bytes& digest)> mfi_sign;
        // 2 => SHA-1/20-byte digests, 3 => SHA-256/32-byte.
        std::function<int()> mfi_protocol_major;

        // Where the accessory identity and known phones live. Empty keeps the
        // old behaviour -- a fresh identity per run, so every phone re-pairs
        // and pair-verify's signature check cannot be enforced.
        std::string state_dir;
    };

    explicit PairingSession(Config config);
    ~PairingSession();

    PairingSession(const PairingSession&) = delete;
    PairingSession& operator=(const PairingSession&) = delete;

    // Each takes one request and returns the response to send. A protocol error
    // is reported as a TLV error inside a 200, which is what the pairing
    // protocol expects -- an RTSP-level error code aborts the connection
    // instead of letting the phone retry.
    rtsp::Message handlePairSetup(const rtsp::Message& request);
    rtsp::Message handlePairVerify(const rtsp::Message& request);
    rtsp::Message handleAuthSetup(const rtsp::Message& request);

    // True once pair-setup M6 has been sent.
    bool paired() const;

    // True when pair-verify authenticated the phone against a key that was on
    // file *before* this session started, rather than one it handed over
    // moments earlier. This is the only form of the check that means anything.
    bool recognised() const;
    // True once pair-verify M4 has been sent. Everything on the control channel
    // after that point is encrypted.
    bool verified() const;

    // Control-channel keys, valid once verified(). Named from HAP's point of
    // view -- the *controller* reads with the "read" key, so that is what the
    // accessory writes with. Getting this backwards looks like a bad key.
    const Bytes& controlReadKey() const;
    const Bytes& controlWriteKey() const;

    // The pair-verify shared secret. The event channel derives its own keys
    // from this with different labels, so it has to be reachable from outside.
    const Bytes& verifySharedSecret() const;

    // CarPlay has no screen to show a setup code on, so pair-setup runs in the
    // "transient" mode with a well-known password. 3939 is what Apple's own
    // transient AirPlay pairing uses; AIRPLAY_SETUP_PASSWORD overrides it while
    // that is being confirmed against hardware.
    static std::string setupPassword();

  private:
    struct State;
    std::unique_ptr<State> state_;
    Config config_;
};

}  // namespace airplay

#endif  // AIRPLAY_PAIRING_SESSION_H_
