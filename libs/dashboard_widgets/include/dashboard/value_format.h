#ifndef DASHBOARD_VALUE_FORMAT_H_
#define DASHBOARD_VALUE_FORMAT_H_

#include <algorithm>

#include <QString>

namespace dashboard
{

// The magnitude a readout will print. Past this a reading is a fault, and the
// string it would make is wider than any readout.
inline constexpr double kReadoutLimit = 1.0e9;

// A reading as fixed-point text with `decimals` places, the way every numeric
// readout prints one.
//
// QString::number renders -0.4 at zero decimals as "-0", and a readout showing
// a signed zero looks like a fault rather than a rounding; that is stripped
// here. It was fixed in one readout and not the other, so the same reading
// printed differently depending on which widget showed it.
inline QString formatFixed(double value, int decimals)
{
    QString text = QString::number(std::clamp(value, -kReadoutLimit, kReadoutLimit), 'f', decimals);
    if (text.startsWith('-') && text.mid(1).toDouble() == 0.0)
    {
        text.remove(0, 1);
    }
    return text;
}

}  // namespace dashboard

#endif  // DASHBOARD_VALUE_FORMAT_H_
