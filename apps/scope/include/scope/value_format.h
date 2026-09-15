#ifndef SCOPE_VALUE_FORMAT_H_
#define SCOPE_VALUE_FORMAT_H_

#include <QString>

#include <cmath>

namespace scope
{

// How a number is printed anywhere in this app.
//
// SHARED SO THE PANELS CANNOT DISAGREE. A plot's axis label, its legend and a
// table cell all print the same reading, and two of them showing "12.5" while
// the third shows "12.48" reads as two different signals rather than as two
// formatters. It is a fifteen-line ladder, which is exactly the sort of thing
// that gets copied and then edited on one side only.
//
// `decimals` below zero picks a width from the magnitude, which keeps a value
// readable across the range a signal actually moves through: rpm wants no
// decimals, a lambda reading wants two. Zero and above is taken literally, for
// a caller that would rather have a column of digits that does not change width
// as the value crosses ten.
inline QString formatValue(double value, int decimals = -1)
{
    if (decimals >= 0)
    {
        return QString::number(value, 'f', decimals);
    }

    const double magnitude = std::abs(value);
    if (magnitude >= 1000.0)
    {
        return QString::number(value, 'f', 0);
    }
    if (magnitude >= 10.0)
    {
        return QString::number(value, 'f', 1);
    }
    if (magnitude >= 0.1 || magnitude == 0.0)
    {
        return QString::number(value, 'f', 2);
    }
    return QString::number(value, 'g', 3);
}

}  // namespace scope

#endif  // SCOPE_VALUE_FORMAT_H_
