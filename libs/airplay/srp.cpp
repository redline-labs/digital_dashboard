// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/stack/srp.ts
#include "airplay/srp.h"

#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <spdlog/spdlog.h>

#include "airplay/crypto.h"

namespace airplay::srp
{

// RFC 5054 3072-bit group, copied from srp.ts (re-wrapped to 64 hex digits per
// line; the value is byte-for-byte the RFC 5054 3072-bit modulus).
const char kModulusHex[] =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
    "98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
    "9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
    "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33"
    "A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864"
    "D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E2"
    "08E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";

namespace
{

constexpr size_t kSaltBytes = 16;
constexpr size_t kPrivateBytes = 32;
constexpr size_t kHashBytes = 64;

struct BnDeleter
{
    void operator()(BIGNUM* n) const { BN_clear_free(n); }
};
struct BnCtxDeleter
{
    void operator()(BN_CTX* c) const { BN_CTX_free(c); }
};

using BnPtr = std::unique_ptr<BIGNUM, BnDeleter>;
using BnCtxPtr = std::unique_ptr<BN_CTX, BnCtxDeleter>;

BnPtr makeBn()
{
    return BnPtr(BN_new());
}

// Minimal big-endian encoding, matching srp.ts toBuf().
Bytes toBytes(const BIGNUM* n)
{
    const int length = BN_num_bytes(n);
    if (length <= 0)
    {
        return {};
    }
    Bytes out(static_cast<size_t>(length));
    BN_bn2bin(n, out.data());
    return out;
}

// Fixed-width left-padded encoding, matching srp.ts pad(): every value that is
// hashed or put on the wire is 384 bytes wide.
Bytes padBytes(const BIGNUM* n)
{
    Bytes out(kModulusBytes);
    if (BN_bn2binpad(n, out.data(), static_cast<int>(kModulusBytes)) < 0)
    {
        SPDLOG_ERROR("[airplay] srp: value wider than the {}-byte modulus", kModulusBytes);
        return {};
    }
    return out;
}

BnPtr fromBytes(const Bytes& b)
{
    BnPtr n = makeBn();
    if (!n || BN_bin2bn(b.data(), static_cast<int>(b.size()), n.get()) == nullptr)
    {
        return nullptr;
    }
    return n;
}

const Bytes& emptyBytes()
{
    static const Bytes empty;
    return empty;
}

// The group parameters and the derived constants that never change.
struct Group
{
    BnPtr N;
    BnPtr g;
    BnPtr k;      // H(N | PAD(g))
    Bytes n_raw;  // toBuf(N)
    Bytes h_xor;  // H(N) XOR H(g), with H(g) taken over the *unpadded* g (0x05)
    bool ok = false;
};

const Group& group()
{
    static const Group instance = []
    {
        Group grp;
        BIGNUM* n = nullptr;
        if (BN_hex2bn(&n, kModulusHex) == 0)
        {
            SPDLOG_ERROR("[airplay] srp: failed to parse the group modulus");
            return grp;
        }
        grp.N.reset(n);

        grp.g = makeBn();
        if (!grp.g || BN_set_word(grp.g.get(), 5) != 1)
        {
            SPDLOG_ERROR("[airplay] srp: failed to build the generator");
            return grp;
        }

        grp.n_raw = toBytes(grp.N.get());
        const Bytes g_padded = padBytes(grp.g.get());
        const Bytes k_hash = crypto::sha512({grp.n_raw, g_padded});
        grp.k = fromBytes(k_hash);
        if (!grp.k)
        {
            SPDLOG_ERROR("[airplay] srp: failed to derive k");
            return grp;
        }

        // srp.ts hashes the *minimal* encoding of g here (a single 0x05 byte),
        // not the padded one - keep that, it is what the HAP clients expect.
        const Bytes h_n = crypto::sha512({grp.n_raw});
        const Bytes h_g = crypto::sha512({toBytes(grp.g.get())});
        if (h_n.size() != kHashBytes || h_g.size() != kHashBytes)
        {
            SPDLOG_ERROR("[airplay] srp: failed to hash the group parameters");
            return grp;
        }
        grp.h_xor.resize(kHashBytes);
        for (size_t i = 0; i < kHashBytes; ++i)
        {
            grp.h_xor[i] = static_cast<uint8_t>(h_n[i] ^ h_g[i]);
        }

        grp.ok = true;
        return grp;
    }();
    return instance;
}

// x = H(salt | H(I | ":" | p))
BnPtr computeX(std::string_view username, std::string_view password, const Bytes& salt)
{
    const Bytes identity(username.begin(), username.end());
    const Bytes secret(password.begin(), password.end());
    const Bytes inner = crypto::sha512({identity, {static_cast<uint8_t>(':')}, secret});
    if (inner.size() != kHashBytes)
    {
        SPDLOG_ERROR("[airplay] srp: identity hash failed");
        return nullptr;
    }
    const Bytes outer = crypto::sha512({salt, inner});
    if (outer.size() != kHashBytes)
    {
        SPDLOG_ERROR("[airplay] srp: x hash failed");
        return nullptr;
    }
    return fromBytes(outer);
}

// Constant-time exponentiation for anything that touches a secret exponent.
bool modExpSecret(BIGNUM* out, const BIGNUM* base, const BIGNUM* exponent, const BIGNUM* modulus, BN_CTX* ctx)
{
    BnPtr safe_exponent(BN_dup(exponent));
    if (!safe_exponent)
    {
        return false;
    }
    BN_set_flags(safe_exponent.get(), BN_FLG_CONSTTIME);
    return BN_mod_exp(out, base, safe_exponent.get(), modulus, ctx) == 1;
}

bool equalConstantTime(const Bytes& a, const Bytes& b)
{
    return a.size() == b.size() && !a.empty() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

// M1 = H(H(N) XOR H(g) | H(I) | salt | PAD(A) | PAD(B) | K)
Bytes computeM1(std::string_view username, const Bytes& salt, const Bytes& padded_a, const Bytes& padded_b,
                const Bytes& session_key)
{
    const Bytes identity(username.begin(), username.end());
    const Bytes h_identity = crypto::sha512({identity});
    return crypto::sha512({group().h_xor, h_identity, salt, padded_a, padded_b, session_key});
}

}  // namespace

std::optional<Bytes> computeVerifier(std::string_view username, std::string_view password, const Bytes& salt)
{
    const Group& grp = group();
    BnCtxPtr ctx(BN_CTX_new());
    BnPtr x = computeX(username, password, salt);
    BnPtr v = makeBn();
    if (!grp.ok || !ctx || !x || !v)
    {
        SPDLOG_ERROR("[airplay] srp: computeVerifier setup failed (salt {} bytes)", salt.size());
        return std::nullopt;
    }

    if (!modExpSecret(v.get(), grp.g.get(), x.get(), grp.N.get(), ctx.get()))
    {
        SPDLOG_ERROR("[airplay] srp: computeVerifier modexp failed");
        return std::nullopt;
    }
    return padBytes(v.get());
}

// --- Server -----------------------------------------------------------------

struct Server::Impl
{
    bool ok = false;
    std::string username;
    Bytes salt;
    Bytes public_b;   // PAD(B)
    Bytes verifier;   // PAD(v)
    BnPtr v;
    BnPtr b;
    BnPtr B;
};

Server::Server(std::string_view username, std::string_view password)
    : Server(username, password, crypto::randomBytes(kSaltBytes), crypto::randomBytes(kPrivateBytes))
{
}

Server::Server(std::string_view username, std::string_view password, const Bytes& salt, const Bytes& private_b)
    : impl_(std::make_unique<Impl>())
{
    const Group& grp = group();
    if (!grp.ok || salt.empty() || private_b.empty())
    {
        SPDLOG_ERROR("[airplay] srp: server setup rejected (salt {} bytes, b {} bytes)", salt.size(),
                     private_b.size());
        return;
    }

    impl_->username = std::string(username);
    impl_->salt = salt;

    BnCtxPtr ctx(BN_CTX_new());
    BnPtr x = computeX(username, password, salt);
    impl_->v = makeBn();
    impl_->b = fromBytes(private_b);
    impl_->B = makeBn();
    BnPtr term = makeBn();
    if (!ctx || !x || !impl_->v || !impl_->b || !impl_->B || !term)
    {
        SPDLOG_ERROR("[airplay] srp: server allocation failed");
        return;
    }

    // v = g^x mod N
    if (!modExpSecret(impl_->v.get(), grp.g.get(), x.get(), grp.N.get(), ctx.get()))
    {
        SPDLOG_ERROR("[airplay] srp: verifier modexp failed");
        return;
    }

    // B = (k*v + g^b) mod N
    if (BN_mod_mul(term.get(), grp.k.get(), impl_->v.get(), grp.N.get(), ctx.get()) != 1 ||
        !modExpSecret(impl_->B.get(), grp.g.get(), impl_->b.get(), grp.N.get(), ctx.get()) ||
        BN_mod_add(impl_->B.get(), impl_->B.get(), term.get(), grp.N.get(), ctx.get()) != 1)
    {
        SPDLOG_ERROR("[airplay] srp: B computation failed");
        return;
    }

    impl_->public_b = padBytes(impl_->B.get());
    impl_->verifier = padBytes(impl_->v.get());
    impl_->ok = !impl_->public_b.empty() && !impl_->verifier.empty();
}

Server::~Server() = default;
Server::Server(Server&&) noexcept = default;
Server& Server::operator=(Server&&) noexcept = default;

bool Server::valid() const
{
    return impl_ && impl_->ok;
}

const Bytes& Server::salt() const
{
    return valid() ? impl_->salt : emptyBytes();
}

const Bytes& Server::publicB() const
{
    return valid() ? impl_->public_b : emptyBytes();
}

const Bytes& Server::verifier() const
{
    return valid() ? impl_->verifier : emptyBytes();
}

VerifyResult Server::verify(const Bytes& client_a, const Bytes& client_m1) const
{
    VerifyResult result;
    const Group& grp = group();
    if (!valid() || !grp.ok)
    {
        SPDLOG_ERROR("[airplay] srp: verify called on an invalid server");
        return result;
    }

    BnCtxPtr ctx(BN_CTX_new());
    BnPtr A = fromBytes(client_a);
    BnPtr residue = makeBn();
    BnPtr base = makeBn();
    BnPtr S = makeBn();
    if (!ctx || !A || !residue || !base || !S)
    {
        SPDLOG_ERROR("[airplay] srp: verify allocation failed (A {} bytes)", client_a.size());
        return result;
    }

    // Reject A ≡ 0 (mod N); it forces S to zero for any b.
    if (BN_nnmod(residue.get(), A.get(), grp.N.get(), ctx.get()) != 1 || BN_is_zero(residue.get()))
    {
        SPDLOG_ERROR("[airplay] srp: client A is zero mod N ({} bytes)", client_a.size());
        return result;
    }

    const Bytes padded_a = padBytes(A.get());
    const Bytes u_hash = crypto::sha512({padded_a, impl_->public_b});
    BnPtr u = fromBytes(u_hash);
    if (padded_a.empty() || !u)
    {
        SPDLOG_ERROR("[airplay] srp: u computation failed");
        return result;
    }

    // S = (A * v^u)^b mod N
    if (!modExpSecret(base.get(), impl_->v.get(), u.get(), grp.N.get(), ctx.get()) ||
        BN_mod_mul(base.get(), A.get(), base.get(), grp.N.get(), ctx.get()) != 1 ||
        !modExpSecret(S.get(), base.get(), impl_->b.get(), grp.N.get(), ctx.get()))
    {
        SPDLOG_ERROR("[airplay] srp: S computation failed");
        return result;
    }

    // K = H(S) over the *minimal* encoding of S, as srp.ts does.
    const Bytes session_key = crypto::sha512({toBytes(S.get())});
    const Bytes expected_m1 = computeM1(impl_->username, impl_->salt, padded_a, impl_->public_b, session_key);
    if (session_key.size() != kHashBytes || expected_m1.size() != kHashBytes)
    {
        SPDLOG_ERROR("[airplay] srp: proof hashing failed");
        return result;
    }

    if (!equalConstantTime(expected_m1, client_m1))
    {
        SPDLOG_ERROR("[airplay] srp: client proof M1 mismatch (got {} bytes, expected {} bytes)", client_m1.size(),
                     expected_m1.size());
        return result;
    }

    // M2 = H(PAD(A) | M1 | K)
    result.server_proof = crypto::sha512({padded_a, client_m1, session_key});
    if (result.server_proof.size() != kHashBytes)
    {
        SPDLOG_ERROR("[airplay] srp: M2 hashing failed");
        return result;
    }

    result.session_key = session_key;
    result.ok = true;
    return result;
}

// --- Client -----------------------------------------------------------------

struct Client::Impl
{
    bool ok = false;
    std::string username;
    std::string password;
    Bytes public_a;  // PAD(A)
    Bytes session_key;
    Bytes client_proof;
    BnPtr a;
    BnPtr A;
};

Client::Client(std::string_view username, std::string_view password)
    : Client(username, password, crypto::randomBytes(kPrivateBytes))
{
}

Client::Client(std::string_view username, std::string_view password, const Bytes& private_a)
    : impl_(std::make_unique<Impl>())
{
    const Group& grp = group();
    if (!grp.ok || private_a.empty())
    {
        SPDLOG_ERROR("[airplay] srp: client setup rejected (a {} bytes)", private_a.size());
        return;
    }

    impl_->username = std::string(username);
    impl_->password = std::string(password);

    BnCtxPtr ctx(BN_CTX_new());
    impl_->a = fromBytes(private_a);
    impl_->A = makeBn();
    if (!ctx || !impl_->a || !impl_->A)
    {
        SPDLOG_ERROR("[airplay] srp: client allocation failed");
        return;
    }

    if (!modExpSecret(impl_->A.get(), grp.g.get(), impl_->a.get(), grp.N.get(), ctx.get()))
    {
        SPDLOG_ERROR("[airplay] srp: A computation failed");
        return;
    }

    impl_->public_a = padBytes(impl_->A.get());
    impl_->ok = !impl_->public_a.empty();
}

Client::~Client() = default;
Client::Client(Client&&) noexcept = default;
Client& Client::operator=(Client&&) noexcept = default;

bool Client::valid() const
{
    return impl_ && impl_->ok;
}

const Bytes& Client::publicA() const
{
    return valid() ? impl_->public_a : emptyBytes();
}

Client::Proof Client::computeProof(const Bytes& salt, const Bytes& server_b)
{
    Proof proof;
    const Group& grp = group();
    if (!valid() || !grp.ok)
    {
        SPDLOG_ERROR("[airplay] srp: computeProof called on an invalid client");
        return proof;
    }

    BnCtxPtr ctx(BN_CTX_new());
    BnPtr B = fromBytes(server_b);
    BnPtr residue = makeBn();
    BnPtr x = computeX(impl_->username, impl_->password, salt);
    BnPtr v = makeBn();
    BnPtr base = makeBn();
    BnPtr exponent = makeBn();
    BnPtr S = makeBn();
    if (!ctx || !B || !residue || !x || !v || !base || !exponent || !S)
    {
        SPDLOG_ERROR("[airplay] srp: computeProof allocation failed (B {} bytes)", server_b.size());
        return proof;
    }

    if (BN_nnmod(residue.get(), B.get(), grp.N.get(), ctx.get()) != 1 || BN_is_zero(residue.get()))
    {
        SPDLOG_ERROR("[airplay] srp: server B is zero mod N ({} bytes)", server_b.size());
        return proof;
    }

    const Bytes padded_b = padBytes(B.get());
    const Bytes u_hash = crypto::sha512({impl_->public_a, padded_b});
    BnPtr u = fromBytes(u_hash);
    if (padded_b.empty() || !u)
    {
        SPDLOG_ERROR("[airplay] srp: client u computation failed");
        return proof;
    }

    // S = (B - k*g^x)^(a + u*x) mod N
    if (!modExpSecret(v.get(), grp.g.get(), x.get(), grp.N.get(), ctx.get()) ||
        BN_mod_mul(base.get(), grp.k.get(), v.get(), grp.N.get(), ctx.get()) != 1 ||
        BN_mod_sub(base.get(), B.get(), base.get(), grp.N.get(), ctx.get()) != 1 ||
        BN_mul(exponent.get(), u.get(), x.get(), ctx.get()) != 1 ||
        BN_add(exponent.get(), exponent.get(), impl_->a.get()) != 1 ||
        !modExpSecret(S.get(), base.get(), exponent.get(), grp.N.get(), ctx.get()))
    {
        SPDLOG_ERROR("[airplay] srp: client S computation failed");
        return proof;
    }

    const Bytes session_key = crypto::sha512({toBytes(S.get())});
    const Bytes m1 = computeM1(impl_->username, salt, impl_->public_a, padded_b, session_key);
    if (session_key.size() != kHashBytes || m1.size() != kHashBytes)
    {
        SPDLOG_ERROR("[airplay] srp: client proof hashing failed");
        return proof;
    }

    impl_->session_key = session_key;
    impl_->client_proof = m1;

    proof.ok = true;
    proof.session_key = session_key;
    proof.client_proof = m1;
    return proof;
}

bool Client::checkServerProof(const Bytes& server_m2) const
{
    if (!valid() || impl_->client_proof.empty())
    {
        SPDLOG_ERROR("[airplay] srp: checkServerProof called before computeProof");
        return false;
    }

    const Bytes expected = crypto::sha512({impl_->public_a, impl_->client_proof, impl_->session_key});
    return equalConstantTime(expected, server_m2);
}

}  // namespace airplay::srp
