#include "service_routes.h"

namespace web_console
{

ServiceRoutes::ServiceRoutes() = default;
ServiceRoutes::~ServiceRoutes() = default;

bool ServiceRoutes::available() const
{
    // Both directories ride the same session; either being invalid means there
    // is no bus to enumerate rather than an empty bus.
    return services_.isValid() && nodes_.isValid();
}

}  // namespace web_console
