#ifndef SCOPE_STATE_NAMES_H_
#define SCOPE_STATE_NAMES_H_

#include "pub_sub/schema_registry.h"

#include <QString>

#include <string>
#include <vector>

namespace scope
{

// What the states of a binding are called, if it has any.
//
// SHARED BY EVERY PANEL THAT SHOWS A VALUE, which is why it is here rather than
// inside the plot that first needed it. An enum reaching a panel as a bare
// ordinal is the failure this prevents: `3` where `iap2` was meant, in a lane, in
// a table cell, and in whatever reads a value next. One resolver means the two
// cannot disagree about which fields have names and what those names are.
//
// Resolved from the SCHEMA rather than carried on the BindingCandidate, because
// a workspace loaded from disk never saw a candidate -- and a binding that got
// its names only when dragged would lose them on the next reload, which is the
// sort of difference nobody notices until the labels are missing.
struct StateNames
{
    // The binding names a state rather than a quantity: an enum, or a bool.
    bool is_state = false;

    // Enumerant names in ordinal order. Empty for a state whose numbers have no
    // names -- a lane or a cell forced onto a plain integer -- which is legal
    // and falls back to the number.
    std::vector<QString> names;

    // The name of `value`, or the number itself when it has none. Here rather
    // than in each caller so a plot's lane and a table's cell spell an unnamed
    // ordinal identically.
    QString label(double value) const;
};

// The states of `expression` read against `schema_type`.
//
// Only a bare field (`phase`) or one list element (`values[7]`) resolves. An
// expression doing arithmetic has left the enum's domain: `phase * 2` is a
// number, not a state, and labelling it would be a lie.
//
// NOT CHEAP -- it reads the schema registry and builds a JSON description -- so
// callers resolve once per binding rather than per frame.
StateNames resolveStateNames(pub_sub::schema_type_t schema_type, const std::string& expression);

}  // namespace scope

#endif  // SCOPE_STATE_NAMES_H_
