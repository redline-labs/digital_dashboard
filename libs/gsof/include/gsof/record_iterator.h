// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turning one GSOF record's bytes into its struct.
//
// The walk itself is gsof/tlv.h -- GSOF records and application-file records
// share their framing, so it is written once. This header is the GSOF
// vocabulary over that grammar.
//
// Dispatch is a visitor rather than a std::variant. A variant over all 22
// records would be the size of its largest alternative -- AllSvDetailed, at
// around 300 bytes -- and every record on the bus would pay for that. The
// visitor also happens to be the shape the node wants: one overload per topic.

#ifndef GSOF_RECORD_ITERATOR_H
#define GSOF_RECORD_ITERATOR_H

#include "gsof/error.h"
#include "gsof/record_table.h"
#include "gsof/records.h"
#include "gsof/tlv.h"

namespace gsof
{

// A GSOF record is a TLV record; the type byte is a RecordType when
// is_known_record() says so.
using RawRecord = TlvRecord;
using RecordIterator = TlvIterator;

// Parse one record and hand the result to `visit`, which must accept every
// record struct -- usually as a generic lambda, or as an overload set with a
// generic fallback.
//
// Returns UnknownRecord for a type not in the table, which is expected rather
// than exceptional, and the parse error for one that is in the table but
// malformed. `visit` is not called in either case.
template <typename Visitor>
constexpr Result<void> visit_record(const RawRecord& raw, Visitor&& visit)
{
    switch (static_cast<RecordType>(raw.type))
    {
#define GSOF_RECORD_VISIT(id, Name, snake)                        \
    case RecordType::Name:                                        \
    {                                                             \
        const Result<Name> parsed = Name::parse(raw.body);        \
        if (!parsed.has_value())                                  \
        {                                                         \
            return std::unexpected(parsed.error());               \
        }                                                         \
        visit(*parsed);                                           \
        return {};                                                \
    }
        GSOF_RECORD_TABLE(GSOF_RECORD_VISIT)
#undef GSOF_RECORD_VISIT
    }

    // Any type not in GSOF_RECORD_TABLE. After the switch rather than in a
    // default:, so adding a row without a parser stays a compile error.
    return unknown_record(raw.type);
}

} // namespace gsof

#endif // GSOF_RECORD_ITERATOR_H
