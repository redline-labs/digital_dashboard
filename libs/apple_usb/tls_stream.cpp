// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple_usb/tls_stream.h"

#include <spdlog/spdlog.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

#include <fcntl.h>
#include <poll.h>

#include <cerrno>
#include <chrono>
#include <cstring>

namespace apple_usb
{

namespace
{

std::string opensslError()
{
    const unsigned long code = ERR_get_error();
    if (code == 0)
    {
        return "no OpenSSL error queued";
    }
    char buf[256];
    ERR_error_string_n(code, buf, sizeof(buf));
    return buf;
}

// PEM in memory -> X509. Null on a blob that is not a certificate.
X509* readCertificate(const std::vector<uint8_t>& pem)
{
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr)
    {
        return nullptr;
    }
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return cert;
}

EVP_PKEY* readPrivateKey(const std::vector<uint8_t>& pem)
{
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr)
    {
        return nullptr;
    }
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return key;
}

}  // namespace

struct TlsStream::Impl
{
    std::unique_ptr<ByteStream> inner;
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    int fd = -1;
    bool dead = false;

    ~Impl()
    {
        if (ssl != nullptr)
        {
            SSL_free(ssl);
        }
        if (ctx != nullptr)
        {
            SSL_CTX_free(ctx);
        }
    }
};

TlsStream::TlsStream(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

TlsStream::~TlsStream() = default;

std::unique_ptr<TlsStream> TlsStream::connect(std::unique_ptr<ByteStream> inner, int fd,
                                              const std::vector<uint8_t>& certificate_pem,
                                              const std::vector<uint8_t>& private_key_pem)
{
    if (inner == nullptr || fd < 0)
    {
        return nullptr;
    }

    auto impl = std::make_unique<Impl>();
    impl->inner = std::move(inner);
    impl->fd = fd;

    impl->ctx = SSL_CTX_new(TLS_method());
    if (impl->ctx == nullptr)
    {
        SPDLOG_ERROR("[tls] SSL_CTX_new failed: {}", opensslError());
        return nullptr;
    }

    // The device's certificates are 1024/2048-bit RSA signed with algorithms
    // that OpenSSL 3's default security level rejects outright. The peer is a
    // phone on the other end of a USB cable, reached through our own mux, so the
    // level buys nothing here and only makes the handshake fail.
    SSL_CTX_set_security_level(impl->ctx, 0);
    SSL_CTX_set_min_proto_version(impl->ctx, TLS1_VERSION);

    // iOS does not always send close_notify before dropping the connection;
    // without this OpenSSL 3 reports an unexpected EOF as a protocol error.
    SSL_CTX_set_options(impl->ctx, SSL_OP_IGNORE_UNEXPECTED_EOF);
    // Older iOS versions do not implement RFC 5746 renegotiation info.
    SSL_CTX_set_options(impl->ctx, SSL_OP_LEGACY_SERVER_CONNECT);

    X509* cert = readCertificate(certificate_pem);
    if (cert == nullptr || SSL_CTX_use_certificate(impl->ctx, cert) != 1)
    {
        SPDLOG_ERROR("[tls] could not load the client certificate: {}", opensslError());
        X509_free(cert);
        return nullptr;
    }
    X509_free(cert);

    EVP_PKEY* key = readPrivateKey(private_key_pem);
    if (key == nullptr || SSL_CTX_use_PrivateKey(impl->ctx, key) != 1)
    {
        SPDLOG_ERROR("[tls] could not load the client private key: {}", opensslError());
        EVP_PKEY_free(key);
        return nullptr;
    }
    EVP_PKEY_free(key);

    impl->ssl = SSL_new(impl->ctx);
    if (impl->ssl == nullptr)
    {
        SPDLOG_ERROR("[tls] SSL_new failed: {}", opensslError());
        return nullptr;
    }
    SSL_set_connect_state(impl->ssl);
    // Nothing to verify against: the device certificate is signed by an
    // authority that exists only inside the pair record.
    SSL_set_verify(impl->ssl, SSL_VERIFY_NONE, nullptr);

    // The socket has to be non-blocking for the read path below to be correct.
    // A record's worth of ciphertext does not line up with a caller's read, so
    // OpenSSL routinely holds decrypted bytes that the socket knows nothing
    // about; deciding whether to wait by polling the fd would sit out the whole
    // timeout with data already in hand. Driving SSL_read first and polling only
    // on WANT_READ is the only ordering that cannot stall, and that requires
    // SSL_read to return rather than block.
    const int flags = ::fcntl(impl->fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(impl->fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        SPDLOG_ERROR("[tls] could not make the socket non-blocking: {}", std::strerror(errno));
        return nullptr;
    }

    if (SSL_set_fd(impl->ssl, impl->fd) != 1)
    {
        SPDLOG_ERROR("[tls] SSL_set_fd failed: {}", opensslError());
        return nullptr;
    }

    for (;;)
    {
        const int rc = SSL_do_handshake(impl->ssl);
        if (rc == 1)
        {
            break;
        }
        const int err = SSL_get_error(impl->ssl, rc);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        {
            // The socket is blocking, so these mean the handshake needs another
            // record; poll rather than spin.
            pollfd pfd{impl->fd, static_cast<short>(err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT),
                       0};
            if (::poll(&pfd, 1, 10000) <= 0)
            {
                SPDLOG_ERROR("[tls] handshake stalled waiting on the socket");
                return nullptr;
            }
            continue;
        }
        SPDLOG_ERROR("[tls] handshake failed (ssl error {}): {}", err, opensslError());
        return nullptr;
    }

    SPDLOG_DEBUG("[tls] session up: {} {}", SSL_get_version(impl->ssl),
                 SSL_get_cipher(impl->ssl));
    return std::unique_ptr<TlsStream>(new TlsStream(std::move(impl)));
}

namespace
{

// Waits for the socket to become readable or writable, whichever the SSL error
// asked for. Returns 1 when ready, 0 on timeout, -1 on error.
int waitFor(int fd, int ssl_error, int timeout_ms)
{
    pollfd pfd{fd, static_cast<short>(ssl_error == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN), 0};
    for (;;)
    {
        const int ready = ::poll(&pfd, 1, timeout_ms);
        if (ready < 0 && errno == EINTR)
        {
            continue;
        }
        return ready < 0 ? -1 : (ready == 0 ? 0 : 1);
    }
}

// A write must complete, so it gets a generous ceiling rather than a caller's
// read timeout. Reaching it means the link is wedged.
constexpr int kWriteTimeoutMs = 30000;

}  // namespace

bool TlsStream::sendAll(const uint8_t* data, size_t len)
{
    if (impl_->ssl == nullptr || impl_->dead)
    {
        return false;
    }
    size_t sent = 0;
    while (sent < len)
    {
        const int n = SSL_write(impl_->ssl, data + sent, static_cast<int>(len - sent));
        if (n > 0)
        {
            sent += static_cast<size_t>(n);
            continue;
        }
        const int err = SSL_get_error(impl_->ssl, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        {
            // Non-blocking: the record could not be flushed yet, and a
            // renegotiation can make a write want to read.
            if (waitFor(impl_->fd, err, kWriteTimeoutMs) <= 0)
            {
                SPDLOG_DEBUG("[tls] write stalled with {}/{} bytes sent", sent, len);
                impl_->dead = true;
                return false;
            }
            continue;
        }
        SPDLOG_DEBUG("[tls] write failed (ssl error {})", err);
        impl_->dead = true;
        return false;
    }
    return true;
}

ssize_t TlsStream::recvSome(uint8_t* out, size_t max_len, unsigned timeout_ms)
{
    if (impl_->ssl == nullptr || impl_->dead)
    {
        return -1;
    }

    // SSL_read first, poll second. The reverse -- poll the socket, then read --
    // looks equivalent but is not: OpenSSL buffers whole records, so after a
    // large message is split across reads the remaining plaintext is already in
    // hand while the socket has nothing to report. Polling first would then wait
    // out the full timeout before returning data it was already holding, which
    // is enough delay for the phone to give up on the iAP2 link and reset the
    // carkit connection.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;)
    {
        const int n = SSL_read(impl_->ssl, out, static_cast<int>(max_len));
        if (n > 0)
        {
            return n;
        }

        const int err = SSL_get_error(impl_->ssl, n);
        if (err == SSL_ERROR_ZERO_RETURN)
        {
            impl_->dead = true;
            return -1;  // clean shutdown
        }
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
        {
            SPDLOG_DEBUG("[tls] read failed (ssl error {})", err);
            impl_->dead = true;
            return -1;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return 0;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const int ready = waitFor(impl_->fd, err, static_cast<int>(remaining));
        if (ready == 0)
        {
            return 0;
        }
        if (ready < 0)
        {
            impl_->dead = true;
            return -1;
        }
    }
}

void TlsStream::close()
{
    if (impl_->ssl != nullptr && !impl_->dead)
    {
        // Best effort: the device often drops the connection without waiting.
        SSL_shutdown(impl_->ssl);
    }
    impl_->dead = true;
    if (impl_->inner != nullptr)
    {
        impl_->inner->close();
    }
}

}  // namespace apple_usb
