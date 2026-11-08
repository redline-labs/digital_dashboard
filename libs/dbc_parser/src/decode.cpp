#include "dbc_parser/decode.h"

#include <bit>
#include <cstring>

namespace dbc_parser
{

namespace
{

bool bitAt(std::span<const uint8_t> payload, uint32_t position)
{
    return ((payload[position / 8u] >> (position % 8u)) & 1u) != 0u;
}

} // namespace

std::optional<uint64_t> rawBits(const Signal &signal, std::span<const uint8_t> payload)
{
    if (signal.length == 0u || signal.length > 64u || signal.lastBitIndex() >= payload.size() * 8u)
    {
        return std::nullopt;
    }

    uint64_t raw = 0;
    if (signal.littleEndian)
    {
        // Intel: the start bit is the least significant, and the field runs
        // upwards through the bytes.
        for (uint32_t i = 0; i < signal.length; ++i)
        {
            if (bitAt(payload, signal.startBit + i))
            {
                raw |= uint64_t{1} << i;
            }
        }
        return raw;
    }

    // Motorola: the start bit is the MOST significant, numbered within its byte,
    // and the field runs down that byte and on into the start of the next --
    // the DBC "sawtooth".
    uint32_t position = signal.startBit;
    for (uint32_t i = 0; i < signal.length; ++i)
    {
        raw = (raw << 1u) | (bitAt(payload, position) ? 1u : 0u);
        position = (position % 8u == 0u) ? position + 15u : position - 1u;
    }
    return raw;
}

std::optional<double> decodeSignal(const Signal &signal, std::span<const uint8_t> payload)
{
    const auto raw = rawBits(signal, payload);
    if (!raw)
    {
        return std::nullopt;
    }

    double value = 0.0;
    switch (signal.valueType)
    {
    case SignalValueType::Float:
    {
        if (signal.length != 32u)
        {
            return std::nullopt;
        }
        value = static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(*raw)));
        break;
    }
    case SignalValueType::Double:
    {
        if (signal.length != 64u)
        {
            return std::nullopt;
        }
        value = std::bit_cast<double>(*raw);
        break;
    }
    case SignalValueType::Integer:
    {
        if (signal.isSigned && signal.length < 64u && ((*raw >> (signal.length - 1u)) & 1u) != 0u)
        {
            // Sign-extend from the field's own width.
            const uint64_t extended = *raw | (~uint64_t{0} << signal.length);
            value = static_cast<double>(std::bit_cast<int64_t>(extended));
        }
        else if (signal.isSigned)
        {
            value = static_cast<double>(std::bit_cast<int64_t>(*raw));
        }
        else
        {
            value = static_cast<double>(*raw);
        }
        break;
    }
    }
    return value * signal.scale + signal.offset;
}

std::map<std::string, double> decodeMessage(const Message &message, std::span<const uint8_t> payload)
{
    std::map<std::string, double> out;

    std::optional<uint64_t> group;
    if (const Signal *multiplexor = message.multiplexor())
    {
        group = rawBits(*multiplexor, payload);
    }

    for (const Signal &signal : message.signals)
    {
        if (signal.isMultiplex && (!group || *group != signal.multiplexedGroupIdx))
        {
            continue;
        }
        if (const auto value = decodeSignal(signal, payload))
        {
            out[signal.name] = *value;
        }
    }
    return out;
}

const Message *findMessage(const Database &database, uint32_t id, bool isExtended)
{
    for (const Message &message : database.messages)
    {
        if (message.id == id && message.isExtended == isExtended)
        {
            return &message;
        }
    }
    return nullptr;
}

const Message *findMessageWithSignal(const Database &database, std::string_view signal)
{
    for (const Message &message : database.messages)
    {
        for (const Signal &candidate : message.signals)
        {
            if (candidate.name == signal)
            {
                return &message;
            }
        }
    }
    return nullptr;
}

} // namespace dbc_parser
