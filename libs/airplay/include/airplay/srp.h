// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/stack/srp.ts
#ifndef AIRPLAY_SRP_H_
#define AIRPLAY_SRP_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace airplay::srp
{

using Bytes = std::vector<uint8_t>;

// SRP-6a as used by AirPlay/HAP pair-setup: RFC 5054 3072-bit group, g = 5,
// SHA-512, username "Pair-Setup", password = the on-screen setup code.
//
//   k  = H(N | PAD(g))
//   x  = H(salt | H(I | ":" | p))
//   v  = g^x mod N
//   B  = (k*v + g^b) mod N
//   u  = H(PAD(A) | PAD(B))
//   S  = (A * v^u)^b mod N
//   K  = H(S)
//   M1 = H(H(N) XOR H(g) | H(I) | salt | PAD(A) | PAD(B) | K)
//   M2 = H(PAD(A) | M1 | K)
//
// Every value that goes on the wire is left-padded to 384 bytes.
inline constexpr size_t kModulusBytes = 384;  // 3072 bits
inline constexpr char kPairSetupUsername[] = "Pair-Setup";

// The RFC 5054 3072-bit group, exactly as carried in srp.ts.
extern const char kModulusHex[];

struct VerifyResult
{
    bool ok = false;
    Bytes session_key;   // K = H(S), 64 bytes
    Bytes server_proof;  // M2, 64 bytes
};

// Server side of /pair-setup. Never throws; a failed construction leaves
// valid() false and every accessor empty.
class Server
{
public:
    // Random 16-byte salt and a random 32-byte private exponent b.
    Server(std::string_view username, std::string_view password);

    // Deterministic variant for known-answer tests and replay of captures.
    Server(std::string_view username, std::string_view password, const Bytes& salt, const Bytes& private_b);

    ~Server();
    Server(Server&&) noexcept;
    Server& operator=(Server&&) noexcept;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool valid() const;

    const Bytes& salt() const;   // 16 bytes
    const Bytes& publicB() const;  // PAD(B), 384 bytes
    const Bytes& verifier() const;  // PAD(v), 384 bytes (diagnostics/tests)

    // Checks the client's public key A and proof M1. On success returns K and M2.
    VerifyResult verify(const Bytes& client_a, const Bytes& client_m1) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Client side. The receiver never needs this on real hardware; it exists so the
// handshake can be exercised end to end in tests.
class Client
{
public:
    // Random 32-byte private exponent a.
    Client(std::string_view username, std::string_view password);
    Client(std::string_view username, std::string_view password, const Bytes& private_a);

    ~Client();
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool valid() const;

    const Bytes& publicA() const;  // PAD(A), 384 bytes

    struct Proof
    {
        bool ok = false;
        Bytes session_key;   // K
        Bytes client_proof;  // M1
    };

    // Consumes the server's salt and B, producing K and M1.
    Proof computeProof(const Bytes& salt, const Bytes& server_b);

    // Confirms the server's M2 against the last computeProof() result.
    bool checkServerProof(const Bytes& server_m2) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Exposed for tests: the SRP verifier for a username/password/salt triple.
std::optional<Bytes> computeVerifier(std::string_view username, std::string_view password, const Bytes& salt);

}  // namespace airplay::srp

#endif  // AIRPLAY_SRP_H_
