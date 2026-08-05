#include "dbc_parser/ast.h"

namespace dbc_parser
{

uint32_t Signal::lastBitIndex() const
{
    if (length == 0)
    {
        return startBit;
    }

    if (littleEndian)
    {
        return startBit + length - 1;
    }

    // Motorola order walks down within a byte and jumps to the top of the next
    // one. This mirrors the walk the generated decoder performs exactly, so
    // what it reports is precisely the highest byte that decoder will index.
    uint32_t absBit = startBit;
    uint32_t highest = startBit;
    for (uint32_t i = 0; i < length; ++i)
    {
        if (absBit > highest)
        {
            highest = absBit;
        }

        if ((absBit % 8u) == 0u)
        {
            absBit += 15u;
        }
        else
        {
            absBit -= 1u;
        }
    }

    return highest;
}

const Signal *Message::multiplexor() const
{
    for (const auto &sig : signals)
    {
        if (sig.isMultiplexor)
        {
            return &sig;
        }
    }
    return nullptr;
}

} // namespace dbc_parser
