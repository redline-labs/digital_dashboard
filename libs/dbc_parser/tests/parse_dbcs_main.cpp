// Dumps what the parser made of a DBC. A debugging aid, not a test -- it is
// deliberately not registered with add_project_test().
#include "dbc_parser/dbc_parser.h"

#include <fstream>
#include <sstream>

#include <spdlog/spdlog.h>

using namespace dbc_parser;

int main(int argc, char **argv)
{
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    if (argc < 2)
    {
        SPDLOG_ERROR("Usage: {} <dbc_file>", argv[0]);
        return 1;
    }

    std::string path = argv[1];
    std::ifstream in(path);
    if (!in)
    {
        SPDLOG_ERROR("Failed to open DBC: {}", path);
        return 1;
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    Parser parser(ss.str());
    auto db = parser.parse();

    for (const auto &entry : parser.diagnostics().entries())
    {
        switch (entry.severity)
        {
        case Severity::Warning:
            SPDLOG_WARN("{}:{}:{}: {}", path, entry.line, entry.column, entry.message);
            break;

        case Severity::Error:
            SPDLOG_ERROR("{}:{}:{}: {}", path, entry.line, entry.column, entry.message);
            break;
        }
    }

    if (!db)
    {
        SPDLOG_ERROR("{} could not be parsed", path);
        return 1;
    }

    SPDLOG_INFO("VERSION: {}", db->version);
    SPDLOG_INFO("Nodes: {}", db->nodes.size());
    for (const auto &n : db->nodes)
    {
        SPDLOG_INFO(" - {}", n);
    }

    SPDLOG_INFO("Value tables: {}", db->valueTables.size());
    for (const auto &[tableName, mappings] : db->valueTables)
    {
        SPDLOG_INFO(" - {} ({} entries)", tableName, mappings.size());
    }

    SPDLOG_INFO("Messages: {}", db->messages.size());
    for (const auto &m : db->messages)
    {
        SPDLOG_INFO(" - id=0x{:X} extended={} name={} dlc={} signals={} multiplexed={} comment={}",
                    m.id, m.isExtended, m.name, m.dlc, m.signals.size(), m.isMultiplexed, m.comment);

        for (const auto &s : m.signals)
        {
            SPDLOG_INFO("   * name={} start={} len={} littleEndian={} signed={} scale={} offset={} "
                        "min={} max={} unit={} receivers={} values={} mux={} muxor={} group={}",
                        s.name, s.startBit, s.length, s.littleEndian, s.isSigned, s.scale, s.offset,
                        s.minimum, s.maximum, s.unit, s.receivers.size(), s.valueTable.size(),
                        s.isMultiplex, s.isMultiplexor, s.multiplexedGroupIdx);

            for (const auto &v : s.valueTable)
            {
                SPDLOG_INFO("       * [{}] = {}", v.rawValue, v.description);
            }
        }
    }

    return 0;
}
