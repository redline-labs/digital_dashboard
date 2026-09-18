#ifndef WEB_CONSOLE_SERVICE_ROUTES_H
#define WEB_CONSOLE_SERVICE_ROUTES_H

#include "http_server.h"

#include "pub_sub/dynamic_service_call.h"
#include "pub_sub/topic_directory.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <string>

// The bus's services, as the console sees them.
//
// Owned by main() rather than by the HTTP server: the directories are liveliness
// subscribers whose whole value is having been watching before a request
// arrives. Constructed once, they know what appeared and disappeared; rebuilt
// per request they would know nothing.
namespace web_console
{

class ServiceRoutes
{
public:
    ServiceRoutes();
    ~ServiceRoutes();

    ServiceRoutes(const ServiceRoutes&) = delete;
    ServiceRoutes& operator=(const ServiceRoutes&) = delete;

    bool available() const;

    const pub_sub::ServiceDirectory& services() const { return services_; }
    const pub_sub::NodeDirectory& nodes() const { return nodes_; }

private:
    pub_sub::ServiceDirectory services_;
    pub_sub::NodeDirectory nodes_;
};

// What a client may ask /api/call to wait. The floor keeps "timeout_ms": 0
// from reporting a live service as gone; the ceiling bounds how long one
// request can hold one of the few HTTP workers.
inline constexpr std::chrono::milliseconds kMinCallTimeout { 100 };
inline constexpr std::chrono::milliseconds kMaxCallTimeout { 10000 };

// Reads /api/call's body into `call`. False, with `error` naming the field,
// for anything the client got wrong -- a wrong-typed field is a 400, never an
// exception out of the handler. timeout_ms is clamped, not refused.
bool parseCallRequest(const nlohmann::json& body, pub_sub::ServiceCallRequest& call,
                      std::string& error);

}  // namespace web_console

#endif  // WEB_CONSOLE_SERVICE_ROUTES_H
