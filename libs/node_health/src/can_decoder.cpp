// SPDX-License-Identifier: GPL-3.0-or-later
#include "node_health/can_decoder.h"

#include "node_health/reporter.h"

#include "cli/interrupt.h"
#include "pub_sub/can_frame.h"
#include "pub_sub/zenoh_subscriber.h"

#include "can_frame.capnp.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <utility>

namespace node_health
{

void runCanDecoder(std::string_view node_name, const std::string& can_key,
                   std::function<bool(const helpers::CanFrame&)> decode)
{
    // Declared before the subscriber, so the subscriber is destroyed first and
    // no callback can touch a check that has gone away.
    HealthReporter health(node_name);
    auto& frames_in = health.addActivityCheck("can_rx", std::chrono::seconds(1));
    auto& decoded = health.addActivityCheck("decoded", std::chrono::seconds(2));

    pub_sub::ZenohTypedSubscriber<CanFrame> can_subscriber(
        can_key,
        [&frames_in, &decoded, decode = std::move(decode)](CanFrame::Reader message)
        {
            frames_in.touch();
            // The real length, not a padded buffer: a frame shorter than the
            // message it claims to be must be rejected, not decoded as though
            // the padding were readings.
            if (decode(pub_sub::fromCapnp(message)))
            {
                decoded.touch();
            }
        });

    health.markReady();
    cli::waitForInterrupt([&health] { health.kick(); });
    SPDLOG_INFO("Interrupted; shutting down.");
}

}  // namespace node_health
