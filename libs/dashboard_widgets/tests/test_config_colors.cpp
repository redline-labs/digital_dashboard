// What a configured colour turns into, and what it must not turn into.
//
// helpers::Color::isValidFormat accepts "#RGB", "#RRGGBB" and "#RRGGBBAA". Qt
// accepts the first two the same way, but reads "#" + 8 hex digits as
// #AARRGGBB. So the one form where the two disagree is exactly the one the
// project documents as supported, and the disagreement is silent: the result is
// a valid QColor, just a different colour than the file asked for.
//
// These assertions are about the channel values rather than about "it parsed",
// because parsing was never the failure.

#include "qt_helpers/widget_colors.h"
#include "helpers/color.h"

#include <cstdio>
#include <string>

namespace
{

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string& what)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

std::string describe(const QColor& c)
{
    return "r=" + std::to_string(c.red()) + " g=" + std::to_string(c.green()) +
           " b=" + std::to_string(c.blue()) + " a=" + std::to_string(c.alpha());
}

// ------------------------------------------------------------ the 8-digit form

void testEightDigitIsReadAsRrggbbaa()
{
    const QColor c = qt_helpers::toQColor(helpers::Color("#112233ff"));

    // Read as #AARRGGBB this is r=0x22 g=0x33 b=0xff a=0x11 -- the bug.
    check(c.red() == 0x11 && c.green() == 0x22 && c.blue() == 0x33 && c.alpha() == 0xff,
          "#112233ff is opaque (0x11,0x22,0x33), got " + describe(c));
}

void testEightDigitAlphaIsHonoured()
{
    const QColor c = qt_helpers::toQColor(helpers::Color("#20304080"));
    check(c.red() == 0x20 && c.green() == 0x30 && c.blue() == 0x40 && c.alpha() == 0x80,
          "#20304080 keeps its alpha, got " + describe(c));
}

void testFullyTransparentIsNotMistakenForInvalid()
{
    // Alpha 00 is a legitimate value, not a parse failure, so it must not fall
    // back. Distinguishable from the fallback only by the RGB channels.
    const QColor c = qt_helpers::toQColor(helpers::Color("#11223300"), Qt::white);
    check(c.red() == 0x11 && c.green() == 0x22 && c.blue() == 0x33 && c.alpha() == 0x00,
          "#11223300 is transparent (0x11,0x22,0x33), not the fallback, got " + describe(c));
}

// ------------------------------------------------------- the unaffected forms

void testShortAndLongHexAreUnchanged()
{
    const QColor three = qt_helpers::toQColor(helpers::Color("#123"));
    check(three.red() == 0x11 && three.green() == 0x22 && three.blue() == 0x33 &&
              three.alpha() == 0xff,
          "#123 expands to (0x11,0x22,0x33) opaque, got " + describe(three));

    const QColor six = qt_helpers::toQColor(helpers::Color("#112233"));
    check(six.red() == 0x11 && six.green() == 0x22 && six.blue() == 0x33 && six.alpha() == 0xff,
          "#112233 is opaque (0x11,0x22,0x33), got " + describe(six));
}

void testQtSpellingsStillWork()
{
    // toQColor is deliberately more permissive than isValidFormat; only the
    // 8-digit form is special-cased, everything else still goes to Qt.
    const QColor named = qt_helpers::toQColor(helpers::Color("red"));
    check(named.red() == 255 && named.green() == 0 && named.blue() == 0,
          "a Qt colour name still parses, got " + describe(named));
}

// ------------------------------------------------------------- the fallback

void testMalformedFallsBack()
{
    const QColor c = qt_helpers::toQColor(helpers::Color("not-a-colour"), Qt::white);
    check(c == QColor(Qt::white), "an unparseable colour falls back, got " + describe(c));

    // Nine characters, so it reaches the 8-digit branch, but the tail is not
    // hex. It has to fall through rather than silently apply a zero alpha.
    const QColor bad_alpha = qt_helpers::toQColor(helpers::Color("#112233zz"), Qt::white);
    check(bad_alpha == QColor(Qt::white),
          "a bad alpha pair falls back rather than parsing as 0, got " + describe(bad_alpha));
}

// --------------------------------------------- agreement with isValidFormat

void testEveryAcceptedFormatSurvives()
{
    // Anything the validator lets into a config has to survive this conversion,
    // or the file was accepted on load and quietly misdrawn afterwards.
    for (const char* text : {"#abc", "#ABC", "#aabbcc", "#AABBCC", "#aabbccdd", "#AABBCCDD"})
    {
        check(helpers::Color::isValidFormat(text),
              std::string(text) + " is a format the validator accepts");

        const QColor c = qt_helpers::toQColor(helpers::Color(text), Qt::magenta);
        check(c != QColor(Qt::magenta),
              std::string(text) + " converts rather than falling back, got " + describe(c));
    }
}

}  // namespace

int main()
{
    testEightDigitIsReadAsRrggbbaa();
    testEightDigitAlphaIsHonoured();
    testFullyTransparentIsNotMistakenForInvalid();
    testShortAndLongHexAreUnchanged();
    testQtSpellingsStillWork();
    testMalformedFallsBack();
    testEveryAcceptedFormatSurvives();

    std::fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
