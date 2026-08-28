// SPDX-License-Identifier: GPL-3.0-or-later
//
// The NAI data-service socket, against itself on the loopback interface.
//
// Labelled `net` because it binds UDP ports: no radio, but not the hermetic
// `unit` set either.
//
// THE NAI PORTS ARE SYMMETRIC: an endpoint binds the well-known port AND sends
// to the same number on the radio, because the radio sends back to that port
// rather than to whatever ephemeral one a client might have taken. So an
// endpoint pointed at 127.0.0.1 talks to itself, and that loopback is the real
// path rather than a contrivance -- encode, socket, receive, decode.

#include "xpr/data_services.h"

#include "mototrbo/nai.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

using namespace std::chrono_literals;

// Not the real TMS port: a test must not collide with a node.
constexpr std::uint16_t kTextPort = 24007;

void checkTextRoundTrip()
{
    xpr::Result<xpr::DataService> service = xpr::DataService::open("127.0.0.1", kTextPort);

    check(service.has_value(), "the endpoint opens");
    if (!service.has_value())
    {
        return;
    }

    check(xpr::send_text(*service, "Hi", 3, true).has_value(), "a text message is sent");

    const xpr::Result<xpr::DataService::Datagram> received = service->receive(500ms);
    check(received.has_value(), "and arrives");
    if (!received.has_value())
    {
        return;
    }

    const mototrbo::Result<mototrbo::nai::tms::TextMessage> decoded =
        mototrbo::nai::tms::parse(received->bytes);
    check(decoded.has_value(), "and decodes");
    if (decoded.has_value())
    {
        check(decoded->text == "Hi", "text survives the round trip");
        check(decoded->sequence == 3, "sequence survives the round trip");
    }

    check(received->sourceAddress == "127.0.0.1", "the sender's address is reported");
}

void checkTimeoutIsNotAnError()
{
    xpr::Result<xpr::DataService> service = xpr::DataService::open("127.0.0.1", kTextPort + 1);
    check(service.has_value(), "an endpoint opens");
    if (!service.has_value())
    {
        return;
    }

    // Silence on a data-service port is normal -- ARS registers on a
    // power-cycle and LRRP answers only if location is enabled -- so it must
    // be distinguishable from a socket that has failed.
    const xpr::Result<xpr::DataService::Datagram> nothing = service->receive(20ms);
    check(!nothing.has_value(), "nothing arrives");
    if (!nothing.has_value())
    {
        check(nothing.error().kind == xpr::Error::Kind::Timeout, "and silence reports as a timeout");
    }
}

void checkBadAddressIsRefused()
{
    xpr::Result<xpr::DataService> service = xpr::DataService::open("not-an-address", kTextPort + 2);
    check(service.has_value(), "the endpoint opens: the address is only used on send");
    if (!service.has_value())
    {
        return;
    }

    check(!xpr::send_text(*service, "x").has_value(), "sending to a bad address fails");
}

} // namespace

int main()
{
    spdlog::set_pattern("[%^%l%$] %v");

    checkTextRoundTrip();
    checkTimeoutIsNotAnError();
    checkBadAddressIsRefused();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }

    SPDLOG_INFO("xpr_test_data_services passed");
    return 0;
}
