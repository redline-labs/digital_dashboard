// SPDX-License-Identifier: GPL-3.0-or-later
//
// ReflectedForm writes each edit into the struct it was built over, and no
// value its editors accept can wrap on the way in.
//
// The integer case is the regression: the editor ranged an unsigned field to
// INT_MAX and read it back with static_cast, so 70000 typed into a uint16_t
// saved as 4464 -- in range, so nothing flagged it.

#include "qt_helpers/reflected_form.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>

#include <cstdio>
#include <string>
#include <string_view>

// A string leaf with a rule, standing in for a topic key.
struct NoSpaces
{
    static constexpr std::string_view kTypeName = "no_spaces";
    static constexpr std::string_view kFormatHint = "no spaces";
    std::string text;
    NoSpaces() = default;
    explicit NoSpaces(std::string value) : text(std::move(value)) {}
    const std::string& str() const { return text; }
    bool operator==(const NoSpaces&) const = default;
    static std::string problem(std::string_view value)
    {
        return value.find(' ') == std::string_view::npos ? std::string() : std::string("has a space");
    }
};

REFLECT_ENUM(form_mode_t, off, on)

REFLECT_STRUCT(form_inner_t,
    (std::string, label, "", "Label"),
    (uint16_t, count, 0, "Count")
)

REFLECT_STRUCT(form_config_t,
    (uint16_t, small, 0, "Small"),
    (int64_t, wide, 0, "Wide"),
    (bool, flag, false, "Flag"),
    (form_mode_t, mode, form_mode_t::off, "Mode"),
    (NoSpaces, key, NoSpaces{}, "Key"),
    (helpers::Color, colour, "#000000", "Colour"),
    (std::vector<uint32_t>, markers, {}, "Markers"),
    (form_inner_t, inner, form_inner_t{}, "Inner"),
    (std::vector<form_inner_t>, rows, {}, "Rows")
)

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

}  // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    form_config_t config;
    config.markers = {10, 20};
    qt_helpers::ReflectedForm form(config);
    int edits = 0;
    QObject::connect(&form, &qt_helpers::ReflectedForm::edited, [&] { ++edits; });

    auto* small = form.findChild<QSpinBox*>("field:small");
    expect(small != nullptr, "a uint16_t gets a spin box");
    if (small != nullptr)
    {
        expect(small->maximum() == 65535 && small->minimum() == 0, "ranged to uint16_t, not to int");
        small->setValue(70000);
        expect(config.small == 65535, "70000 is held at 65535, not wrapped to 4464");
    }

    auto* wide = form.findChild<QDoubleSpinBox*>("field:wide");
    if (wide != nullptr)
    {
        wide->setValue(1e30);
        expect(config.wide == 9007199254740992LL, "an int64 is held at 2^53, the last exact double");
    }
    else
    {
        expect(false, "an int64 gets a wide spin box");
    }

    form.findChild<QCheckBox*>("field:flag")->setChecked(true);
    expect(config.flag, "a check box writes its bool");
    form.findChild<QComboBox*>("field:mode")->setCurrentText("on");
    expect(config.mode == form_mode_t::on, "a combo writes its enum");
    form.findChild<QLineEdit*>("field:inner.label")->setText("x");
    expect(config.inner.label == "x", "a nested struct's field is written through its path");

    auto* key = form.findChild<QLineEdit*>("field:key");
    key->setText("a b");
    expect(config.key.str() == "a b", "a ruled field is written as typed");
    expect(form.problems().size() == 1 && form.problems().front().startsWith("key:"),
           "and its problem is reported by path");
    key->setText("ab");
    expect(form.problems().isEmpty(), "and cleared when fixed");

    form.findChild<QPushButton*>("field:markers:add")->click();
    expect(config.markers.size() == 3, "Add appends an entry");
    QApplication::processEvents();
    form.findChild<QPushButton*>("field:markers[0]:remove")->click();
    QApplication::processEvents();
    expect(config.markers.size() == 2 && config.markers[0] == 20, "Remove takes out that entry");
    auto* first = form.findChild<QDoubleSpinBox*>("field:markers[0]");
    if (first != nullptr)
    {
        first->setValue(7);
        expect(config.markers[0] == 7, "a rebuilt row writes the element now at its index");
    }
    else
    {
        expect(false, "the list was rebuilt after a remove");
    }

    form.findChild<QPushButton*>("field:rows:add")->click();
    expect(config.rows.size() == 1, "a list of structs adds an element");
    if (auto* count = form.findChild<QSpinBox*>("field:rows[0].count"))
    {
        count->setValue(3);
        expect(config.rows[0].count == 3, "and its form writes into it");
    }
    else
    {
        expect(false, "the selected element gets its own form");
    }

    expect(edits > 0, "edits are signalled");
    std::fprintf(stderr, "%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
