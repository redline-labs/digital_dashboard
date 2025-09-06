#include "dbc_parser/dbc_parser.h"

#include <fstream>
#include <iostream>
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
    Parser p(ss.str());
    ParseError err;
    auto db = p.parse(err);
    if (!db)
    {
        SPDLOG_ERROR("Parse error at {}:{}: {}", err.line, err.column, err.message);
        return 1;
    }

    SPDLOG_INFO("VERSION: {}", db->version);
    SPDLOG_INFO("Nodes: {}", db->nodes.size());
    for (const auto &n : db->nodes)
    {
        SPDLOG_INFO(" - {}", n);
    }

    SPDLOG_INFO("Messages: {}", db->messages.size());
    for (const auto &m : db->messages)
    {
        SPDLOG_INFO(" - id=0x{:03X} name={} dlc={} signals={} comment={} isMultiplexed={}", m.id, m.name, m.dlc, m.signals.size(), m.comment, m.isMultiplexed);
        for (const auto &s : m.signals)
        {
            SPDLOG_INFO("   * name={} startBit={} length={} littleEndian={} isSigned={} scale={} offset={} minimum={} maximum={} unit={} receivers={} valueTable={} comment={} isMultiplex={} isMultiplexor={} multiplexedGroupIdx={}", s.name, s.startBit, s.length, s.littleEndian, s.isSigned, s.scale, s.offset, s.minimum, s.maximum, s.unit, s.receivers.size(), s.valueTable.size(), s.comment, s.isMultiplex, s.isMultiplexor, s.multiplexedGroupIdx);
            if (s.valueTable.size() > 0)
            {
                for (const auto &v : s.valueTable)
                {
                    SPDLOG_INFO("       * [{}] = {}", v.rawValue, v.description);
                }
            }
        }
    }

    return 0;
}


