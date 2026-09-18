#ifndef WEB_CONSOLE_SERVICE_ROUTES_H
#define WEB_CONSOLE_SERVICE_ROUTES_H

#include "http_server.h"

#include "pub_sub/topic_directory.h"

#include <memory>

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

}  // namespace web_console

#endif  // WEB_CONSOLE_SERVICE_ROUTES_H
