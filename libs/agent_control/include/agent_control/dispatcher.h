#ifndef AGENT_CONTROL_DISPATCHER_H_
#define AGENT_CONTROL_DISPATCHER_H_

#include "agent_control/error.h"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace agent_control
{

// Whether a method changes application state. A GUI app drains its event loop
// after a mutating method, so a screenshot taken straight after a click
// observes the click's effect rather than the frame before it.
enum class MethodKind
{
    kReadOnly,
    kMutating,
};

// The JSON-RPC 2.0 envelope and the method table, without Qt.
//
// Where a handler runs is not this class's business: the GUI apps hop every
// call to the GUI thread, a simulator to its run loop. Each supplies an
// Invoker, and everything else -- parsing, envelope checks, the id echo,
// _timeout_ms, rpc.methods -- is said once, here.
class Dispatcher
{
  public:
    using Method = std::function<MethodResult(const json& params)>;

    struct Entry
    {
        Method handler;
        MethodKind kind = MethodKind::kReadOnly;
        std::string summary;
    };

    // Runs one call. Must return within roughly timeout_ms; an invoker that
    // hops threads answers kGuiThreadBusy (or similar) rather than waiting
    // longer. Exceptions are caught by the dispatcher either way.
    using Invoker =
        std::function<MethodResult(const std::string& method, const Entry& entry, const json& params, int timeout_ms)>;

    static constexpr int kDefaultTimeoutMs = 5000;

    // No invoker: the handler runs on the caller's thread.
    explicit Dispatcher(std::string app_name, Invoker invoker = {});

    // `name` is dotted and namespaced by area: "ui.snapshot", "can.send".
    // Registering an existing name replaces it. The summary, when given, is
    // listed by rpc.methods.
    void registerMethod(std::string name, Method handler, MethodKind kind = MethodKind::kReadOnly,
                        std::string summary = {});

    // One request line to one response line (no trailing newline).
    std::string handleLine(std::string_view line);

    const std::string& appName() const { return app_name_; }
    std::vector<std::string> methodNames() const;

    // Runs a handler, turning anything it throws into kInternal. For invokers.
    static MethodResult runHandler(const Entry& entry, const json& params);

  private:
    json dispatch(const json& request);

    std::string app_name_;
    Invoker invoker_;

    // Guards the table only: a late registerMethod() must not race a client
    // thread's lookup.
    mutable std::mutex mutex_;
    std::map<std::string, Entry, std::less<>> methods_;
};

}  // namespace agent_control

#endif  // AGENT_CONTROL_DISPATCHER_H_
