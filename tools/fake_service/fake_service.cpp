// A service that answers, so the console's service-call path is developable
// without hardware.
//
// Same reason tools/rauc_stub exists. Every node that offers a real service --
// backlight, can_bridge, msel_master_relay -- wants a device behind it, so on a
// workstation there is nothing to call and /api/call can only ever be exercised
// against an empty bus. This offers one service with a schema that already
// exists, so the whole path is real: liveliness announces it, the directory
// lists it, describeSchema builds a form for it, and callServiceBlocking
// actually gets a reply.
//
// A workstation tool: never shipped, and deliberately not redline_install()ed.

#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_service.h"

#include "display_backlight.capnp.h"

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

int main(int argc, char** argv)
{
    cxxopts::Options options("fake_service", "Answers one service, for developing the web console");
    options.add_options()
        ("k,key", "Service key", cxxopts::value<std::string>()->default_value("nodes/fake_backlight/set_brightness"))
        ("s,seconds", "How long to stay up", cxxopts::value<int>()->default_value("120"))
        ("h,help", "Print usage");

    const auto parsed = options.parse(argc, argv);
    if (parsed.count("help"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    const std::string key = parsed["key"].as<std::string>();

    pub_sub::NodeIdentity identity("fake_backlight");

    pub_sub::ZenohService<DisplayBrightnessRequest, DisplayBrightnessResponse> service(
        key,
        [](const DisplayBrightnessRequest::Reader& request,
           DisplayBrightnessResponse::Builder& response) {
            const double value = request.getValue();
            SPDLOG_INFO("[fake] asked for {}", value);

            if (!(value >= 0.0) || value > 100.0)
            {
                // Services report their own refusals in the response rather than
                // by erroring the query, which is the convention the tree uses.
                response.setOk(false);
                response.setMessage("value must be between 0 and 100");
                return;
            }

            response.setOk(true);
            response.setAppliedRaw(static_cast<uint32_t>(value * 655.35));
            response.setAppliedPercent(static_cast<float>(value));
            response.setMessage("applied by the fake responder");
        });

    SPDLOG_INFO("[fake] offering {}", key);
    std::this_thread::sleep_for(std::chrono::seconds(parsed["seconds"].as<int>()));
    return 0;
}
