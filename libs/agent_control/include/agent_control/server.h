#ifndef AGENT_CONTROL_SERVER_H_
#define AGENT_CONTROL_SERVER_H_

#include "agent_control/error.h"
#include "agent_control/locator.h"

#include <QObject>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace agent_control
{

class ControlSocket;

// The agent-facing control endpoint embedded in an application.
//
// Speaks JSON-RPC 2.0 over newline-delimited unix-socket frames. It deliberately
// does not speak MCP: tool names, descriptions and argument schemas live in the
// companion Python server where they can change without a rebuild. What lives
// here is only the set of things the app can actually do.
//
// Handlers registered here are ALWAYS invoked on the GUI thread, so they may
// touch widgets directly without any further marshalling.
class AgentServer : public QObject
{
    Q_OBJECT

  public:
    // Whether a method changes application state. Mutating methods drain the
    // event loop before returning, so a screenshot taken straight after a click
    // observes the click's effect rather than the frame before it.
    enum class MethodKind
    {
        kReadOnly,
        kMutating,
    };

    using Method = std::function<MethodResult(const json& params)>;

    // Wall-clock budget for a single method to run on the GUI thread. Exceeding
    // it yields GUI_THREAD_BUSY, which is a real answer about the app's health
    // rather than a hung caller. Override per call with params["_timeout_ms"].
    static constexpr int kDefaultTimeoutMs = 5000;

    explicit AgentServer(std::string app_name, QObject* parent = nullptr);
    ~AgentServer() override;

    // `name` is dotted and namespaced by area: "ui.snapshot", "input.click".
    // Registering an existing name replaces it.
    void registerMethod(std::string name, Method handler, MethodKind kind = MethodKind::kReadOnly);

    bool start(const std::string& socket_path);
    void stop();

    const std::string& appName() const { return app_name_; }
    std::string socketPath() const;
    std::vector<std::string> methodNames() const;

    // Owned here so there is exactly one ref table and one revision counter per
    // application, shared by every method that addresses a widget. Only touch it
    // from the GUI thread (i.e. from inside a handler).
    WidgetLocator& locator() { return locator_; }

    // Exposed so tests can drive the dispatcher without a socket.
    std::string handleLine(std::string_view line);

  private:
    struct Entry
    {
        Method handler;
        MethodKind kind = MethodKind::kReadOnly;
    };

    json dispatch(const json& request);
    MethodResult invoke(const std::string& method, const json& params, int timeout_ms);
    void registerBuiltins();

    std::string app_name_;
    WidgetLocator locator_;
    std::unique_ptr<ControlSocket> socket_;

    // Guards the table only. Handlers run on the GUI thread; registration
    // happens at startup but a late registerMethod() must not race a client
    // thread's lookup.
    mutable std::mutex methods_mutex_;
    std::map<std::string, Entry, std::less<>> methods_;
};

}  // namespace agent_control

#endif  // AGENT_CONTROL_SERVER_H_
