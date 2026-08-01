// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef APPLE_USB_TLS_STREAM_H_
#define APPLE_USB_TLS_STREAM_H_

#include "apple_usb/byte_stream.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace apple_usb
{

// A client-certificate TLS session over an already-connected stream.
//
// Two places need it and they are the same handshake: lockdown after
// StartSession answers EnableSessionSSL, and a service connection when
// StartService answers EnableServiceSSL. Both present the pair record's root
// certificate and key, and neither verifies the peer -- the device's certificate
// is self-signed by an authority that only exists inside the pair record, so
// there is nothing to chain it to. Trust here comes from the pairing having
// happened at all, not from the certificate.
class TlsStream : public ByteStream
{
  public:
    ~TlsStream() override;

    TlsStream(const TlsStream&) = delete;
    TlsStream& operator=(const TlsStream&) = delete;

    // Wraps `inner` and runs the handshake. Returns nullptr on failure, in which
    // case `inner` is released along with the failed session. The PEM blobs come
    // from the pair record.
    static std::unique_ptr<TlsStream> connect(std::unique_ptr<ByteStream> inner,
                                              int fd,
                                              const std::vector<uint8_t>& certificate_pem,
                                              const std::vector<uint8_t>& private_key_pem);

    bool sendAll(const uint8_t* data, size_t len) override;
    ssize_t recvSome(uint8_t* out, size_t max_len, unsigned timeout_ms) override;
    void close() override;

  private:
    struct Impl;
    explicit TlsStream(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace apple_usb

#endif  // APPLE_USB_TLS_STREAM_H_
