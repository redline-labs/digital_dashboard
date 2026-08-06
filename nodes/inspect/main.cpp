#include "inspect/verbs.h"

#include "cli/program.h"

#include <array>

namespace
{

// THE verb table. Adding a verb is one row here; see inspect/verbs.h.
//
// Ordered by how often they are reached for rather than alphabetically, because
// this is also the order the verb list prints in.
constexpr std::array<cli::Verb, 12> kInspectVerbs{{
    {"list", "What is on the bus, whether or not it has published yet", inspect::addListOptions,
     inspect::runList},
    {"info", "Everything known about one key: schema, fields, owner", inspect::addInfoOptions,
     inspect::runInfo},
    {"echo", "Print messages as they arrive, decoded", inspect::addEchoOptions, inspect::runEcho},
    {"watch", "Live table of the whole bus: rate, bandwidth, owner", inspect::addWatchOptions,
     inspect::runWatch},
    {"hz", "Message rate, with the distribution of the gaps", inspect::addHzOptions,
     inspect::runHz},
    {"bw", "Bandwidth per key", inspect::addBwOptions, inspect::runBw},
    {"latency", "Transport delay: arrival minus the publisher's stamp", inspect::addLatencyOptions,
     inspect::runLatency},
    {"nodes", "Our processes, by name", inspect::addNodesOptions, inspect::runNodes},
    {"services", "Callable services and their request/response schemas",
     inspect::addServicesOptions, inspect::runServices},
    {"call", "Call a service with a JSON request", inspect::addCallOptions, inspect::runCall},
    {"schema", "The schema registry -- needs no bus", inspect::addSchemaOptions,
     inspect::runSchema},
    {"publish", "Publish a message built from JSON", inspect::addPublishOptions,
     inspect::runPublish},
}};

}  // namespace

int main(int argc, char** argv)
{
    const cli::Program program("inspect", "Inspect the zenoh bus", kInspectVerbs);
    return program.run(argc, argv);
}
