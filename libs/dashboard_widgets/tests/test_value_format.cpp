// The fixed-point text every numeric readout prints. The signed zero is the
// case with history: QString::number renders -0.4 at zero decimals as "-0",
// one readout stripped it and the other did not.

#include "dashboard/value_format.h"

#include <cstdio>
#include <string>

namespace
{

int g_failures = 0;
int g_checks = 0;

void expectText(double value, int decimals, const char* expected)
{
    ++g_checks;
    const QString got = dashboard::formatFixed(value, decimals);
    if (got != QString::fromLatin1(expected))
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: formatFixed(%g, %d) = '%s', expected '%s'\n", value, decimals,
                     got.toStdString().c_str(), expected);
    }
}

}  // namespace

int main()
{
    expectText(-0.4, 0, "0");      // not "-0"
    expectText(-0.04, 1, "0.0");   // not "-0.0"
    expectText(-0.6, 0, "-1");     // a real negative keeps its sign
    expectText(12.345, 1, "12.3");
    expectText(5.0, 2, "5.00");
    expectText(3.0e12, 0, "1000000000");  // clamped to the readout limit
    std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
