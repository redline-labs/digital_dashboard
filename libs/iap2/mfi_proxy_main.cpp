// SPDX-License-Identifier: GPL-3.0-or-later
//
// mfi_proxy: the MFi authentication coprocessor, served over HTTP.
//
// WHAT IT IS FOR. The coprocessor is soldered to one board. The rest of the
// CarPlay stack is portable, so a bring-up problem that wants a laptop's
// tooling is otherwise pinned to whichever machine owns the chip. Run this on
// the board that has it and point a carplay node anywhere else at it with
// --mfi-remote; see iap2/http_mfi_signer.h for the wire format.
//
// THIS IS A BENCH TOOL. Signing is the step that proves to a phone that a
// licensed Apple accessory is on the far end of the cable, and serving it lets
// anyone who can reach the port borrow that proof, with no way for the chip or
// for Apple to tell them apart from us.
//
// So it is NOT in the image: there is no redline_install for this target and no
// unit for it, and a production build does not carry it. It is deployed by
// copying the binary to the board's /data, which survives an image update --
// see docs/nodes/carplay.md.
//
// That is the cautious end of a real judgement call, and the argument the other
// way is worth recording: running the proxy requires being root on the board,
// and root on the board can already talk to /dev/i2c-13 directly -- the chip
// has no access control of its own. What the proxy adds is reach, and reach is
// a deliberate act by whoever starts it. If carrying the binary in the image
// turns out to be worth more than the copy step costs, that is the reasoning to
// revisit.
//
// The defaults follow regardless. It binds to 127.0.0.1 unless told otherwise
// (over an SSH tunnel it needs nothing more), it says so loudly when told
// otherwise, and --token exists so that "otherwise" can at least require a
// shared secret.

#include "iap2/mcp2221a_mfi_signer.h"

#include "core/core.h"
#include "httplib_wrapped/httplib_include.h"

#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{

// The chip is one piece of hardware behind one I2C bus, and httplib answers on
// a pool of threads. Every route below takes this before touching the signer --
// which is the same rule usb_pipeline follows in-process with the mutex it puts
// in SessionContext, for the same reason.
std::mutex g_chip_mutex;

// Set so the signal handler can wake listen() up; httplib::Server::stop is
// documented as safe from another thread.
std::atomic<httplib::Server*> g_server{nullptr};

void handleSignal(int)
{
    httplib::Server* server = g_server.load();
    if (server != nullptr)
    {
        server->stop();
    }
}

// A challenge is 20 bytes (SHA-1, protocol 2) or 32 (SHA-256, protocol 3).
// Nothing legitimate is larger, and the cap keeps a stray POST from buffering.
constexpr size_t kMaxBody = 1024;

void setBytes(httplib::Response& response, const std::vector<uint8_t>& bytes)
{
    response.set_content(reinterpret_cast<const char*>(bytes.data()), bytes.size(),
                         "application/octet-stream");
}

// Non-200 bodies are text, because the only reader is a human tailing a log or
// HttpMfiSigner, which logs whatever it is given.
void fail(httplib::Response& response, int status, const std::string& why)
{
    response.status = status;
    response.set_content(why, "text/plain");
}

}  // namespace

int main(int argc, char** argv)
{
    core::setupLogging({.program = "mfi_proxy"});

    cxxopts::Options options("mfi_proxy", "Serve the MFi coprocessor over HTTP (bench tool)");
    options.add_options()
        ("bind", "Address to listen on. The default keeps the chip reachable only from this "
                 "machine; forward a port over SSH rather than changing it if you can",
         cxxopts::value<std::string>()->default_value("127.0.0.1"))
        ("port", "TCP port to listen on", cxxopts::value<int>()->default_value("8099"))
        ("mfi-i2c-device",
         "I2C adapter of the MFi coprocessor, e.g. /dev/i2c-13 (default: $REDLINE_MFI_I2C_DEV, "
         "else auto-detect)",
         cxxopts::value<std::string>()->default_value(""))
        ("token", "Shared secret required as \"Authorization: Bearer <token>\". Empty (the "
                  "default) accepts any caller, which is only sane on loopback",
         cxxopts::value<std::string>()->default_value(""))
        ("v,verbose", "Enable debug logging")
        ("h,help", "Print usage");

    const auto args = options.parse(argc, argv);
    if (args.count("help"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }
    if (args.count("verbose"))
    {
        spdlog::set_level(spdlog::level::debug);
    }

    const auto bind_address = args["bind"].as<std::string>();
    const auto port = args["port"].as<int>();
    const auto token = args["token"].as<std::string>();

    if (bind_address != "127.0.0.1" && bind_address != "localhost" && bind_address != "::1")
    {
        SPDLOG_WARN("[mfi] listening on {} -- anyone who can reach this port can sign with our "
                    "MFi chip{}", bind_address,
                    token.empty() ? ", and no token is set" : "");
    }

    auto signer = std::make_unique<iap2::Mcp2221aMfiSigner>();
    if (!signer->init(args["mfi-i2c-device"].as<std::string>()))
    {
        // Fatal here, unlike in the node: the node has a dashboard to run
        // without CarPlay, and this process has nothing else to do.
        SPDLOG_ERROR("[mfi] coprocessor unavailable; nothing to serve");
        return 1;
    }
    SPDLOG_INFO("[mfi] coprocessor ready (protocol major {})", signer->protocolMajor());

    httplib::Server server;
    server.set_payload_max_length(kMaxBody);

    // Checked on every route rather than in a pre-routing handler, so that
    // adding a route cannot quietly add an unauthenticated one.
    const auto authorized = [&token](const httplib::Request& request,
                                     httplib::Response& response) {
        if (token.empty())
        {
            return true;
        }
        if (request.get_header_value("Authorization") == "Bearer " + token)
        {
            return true;
        }
        fail(response, 401, "bad or missing bearer token\n");
        return false;
    };

    server.Get("/mfi/v1/protocol", [&](const httplib::Request& request,
                                       httplib::Response& response) {
        if (!authorized(request, response))
        {
            return;
        }
        const std::lock_guard<std::mutex> lock(g_chip_mutex);
        response.set_content(std::to_string(signer->protocolMajor()), "text/plain");
    });

    server.Get("/mfi/v1/certificate", [&](const httplib::Request& request,
                                          httplib::Response& response) {
        if (!authorized(request, response))
        {
            return;
        }
        const std::lock_guard<std::mutex> lock(g_chip_mutex);
        const auto certificate = signer->certificate();
        if (!certificate)
        {
            fail(response, 503, "coprocessor did not return a certificate\n");
            return;
        }
        SPDLOG_INFO("[mfi] served certificate ({} bytes) to {}", certificate->size(),
                    request.remote_addr);
        setBytes(response, *certificate);
    });

    server.Post("/mfi/v1/sign", [&](const httplib::Request& request,
                                    httplib::Response& response) {
        if (!authorized(request, response))
        {
            return;
        }
        if (request.body.empty())
        {
            fail(response, 400, "empty challenge\n");
            return;
        }

        const auto* bytes = reinterpret_cast<const uint8_t*>(request.body.data());
        const std::vector<uint8_t> challenge(bytes, bytes + request.body.size());

        const std::lock_guard<std::mutex> lock(g_chip_mutex);
        const auto signature = signer->signChallenge(challenge);
        if (!signature)
        {
            fail(response, 503, "coprocessor did not sign the challenge\n");
            return;
        }
        SPDLOG_INFO("[mfi] signed a {}-byte challenge for {} ({} bytes out)", challenge.size(),
                    request.remote_addr, signature->size());
        setBytes(response, *signature);
    });

    g_server.store(&server);
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    // Bind and listen as two steps so that "serving on" is only ever printed by
    // a process that actually holds the port.
    //
    // It does not mean this is the ONLY such process. cpp-httplib sets
    // SO_REUSEPORT, so starting a second proxy on the same port succeeds
    // silently and the kernel then splits incoming connections between the two
    // -- which looks, from the client, exactly like a proxy that intermittently
    // ignores its own --token. Check for a stale one before blaming the code.
    if (!server.bind_to_port(bind_address, port))
    {
        SPDLOG_ERROR("[mfi] cannot bind {}:{}", bind_address, port);
        g_server.store(nullptr);
        return 1;
    }

    SPDLOG_INFO("[mfi] serving on {}:{}{}", bind_address, port,
                token.empty() ? "" : " (token required)");
    if (!server.listen_after_bind())
    {
        SPDLOG_ERROR("[mfi] listen on {}:{} failed", bind_address, port);
        g_server.store(nullptr);
        return 1;
    }

    g_server.store(nullptr);
    SPDLOG_INFO("[mfi] stopped");
    return 0;
}
