// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef IAP2_HTTP_MFI_SIGNER_H_
#define IAP2_HTTP_MFI_SIGNER_H_

#include "iap2/mfi_signer.h"

#include <string>

namespace iap2
{

// MfiSigner backed by a coprocessor on ANOTHER machine, reached over HTTP.
//
// WHY THIS EXISTS. The MFi coprocessor is soldered to one board. Everything
// else in the CarPlay stack -- the mux, the iAP2 session, the AirPlay
// server -- runs anywhere, so a failure that needs a laptop's tooling to
// diagnose (usbmon, a debugger, a second phone) is stuck on the board that
// happens to own the chip. This moves the three chip operations across the
// network so the rest of the stack can run where it is convenient, leaving
// mfi_proxy on the board as the chip's only local user.
//
// It is a BENCH TOOL. Signing is the step that proves "a licensed Apple
// accessory is on the other end of this cable", and this hands that proof to
// anyone who can reach the port. Bind the proxy to loopback unless you mean
// otherwise, and do not ship it enabled. See mfi_proxy_main.cpp.
//
// THE WIRE FORMAT is raw bytes, one endpoint per interface method, no JSON and
// no base64 -- every payload here is already an opaque blob, and encoding one
// only adds a place to corrupt it:
//
//   GET  <base>/mfi/v1/protocol      -> 200 text/plain, the major version
//   GET  <base>/mfi/v1/certificate   -> 200 application/octet-stream, DER
//   POST <base>/mfi/v1/sign          -> body is the raw challenge,
//                                       200 application/octet-stream, signature
//
// Any non-200 becomes std::nullopt, which is what the callers already handle
// for a chip that did not answer.
class HttpMfiSigner : public MfiSigner
{
  public:
    // `base_url` is scheme://host:port, e.g. "http://10.0.0.91:8099". A
    // trailing slash is tolerated. `token`, when non-empty, is sent as
    // "Authorization: Bearer <token>" and must match the proxy's --token.
    HttpMfiSigner(std::string base_url, std::string token = {});

    // Fetches the protocol major version, which doubles as a reachability
    // check: the proxy is up, the chip answered, and the two agree on the
    // route names. Must succeed before use, the same way
    // Mcp2221aMfiSigner::init must.
    bool init();

    std::optional<std::vector<uint8_t>> certificate() override;
    std::optional<std::vector<uint8_t>> signChallenge(const std::vector<uint8_t>& challenge) override;
    int protocolMajor() override;

  private:
    // Returns the response body, or nullopt. `body` empty means GET.
    std::optional<std::vector<uint8_t>> request(const char* path,
                                                const std::vector<uint8_t>* body);

    std::string base_url_;
    std::string token_;
    int protocol_major_ = 0;
};

}  // namespace iap2

#endif  // IAP2_HTTP_MFI_SIGNER_H_
