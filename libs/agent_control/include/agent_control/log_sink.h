#ifndef AGENT_CONTROL_LOG_SINK_H_
#define AGENT_CONTROL_LOG_SINK_H_

#include "agent_control/log_ring.h"

#include <cstddef>
#include <memory>

namespace agent_control
{

// installLogRing(), and routes Qt's own diagnostics (qWarning, QPA errors,
// layout complaints) into spdlog under the logger name "qt".
//
// Qt's messages are worth capturing precisely because nothing in this codebase
// captures them today -- they go to stderr and vanish. A screenshot that comes
// back black often has a QPA line behind it.
std::shared_ptr<RingSink> installLogCapture(std::size_t capacity = 5000);

}  // namespace agent_control

#endif  // AGENT_CONTROL_LOG_SINK_H_
