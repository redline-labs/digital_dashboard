// SPDX-License-Identifier: GPL-3.0-or-later
//
// SchemaForm: a request form built at runtime from a capnp schema.
//
// Two things are worth pinning. First, that EVERY schema in the registry gets a
// form whose defaults the send path accepts -- a schema with a field shape the
// form cannot represent should fail here, on the build that adds it, not the
// first time someone selects that service. Second, that setValue()/value() is an
// identity over every field shape, since history replay and the agent
// interface both put JSON in and expect to get the same JSON out.
//
// Mutation-checks:
//   * make IntegerEditor parse through double    -> the uint64-max identity fails
//   * drop the all-or-nothing check in setValue  -> "left unchanged" fails
//   * make ListEditor ignore $fixedLength         -> nothing here (no fixture
//     list is fixed); the registry sweep covers the PDM schemas that are.

#include "switchboard/schema_form.h"

#include "pub_sub/capnp_json.h"
#include "pub_sub/schema_registry.h"

#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/schema-parser.h>
#include <kj/filesystem.h>

#include <spdlog/spdlog.h>

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include <cstdint>
#include <cstdio>
#include <string>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

using switchboard::json;
using switchboard::SchemaForm;

// Row removal goes through deleteLater.
void settle()
{
    for (int i = 0; i < 5; ++i)
    {
        QAbstractEventDispatcher::instance()->processEvents(QEventLoop::AllEvents);
    }
}

bool acceptedBySendPath(capnp::StructSchema schema, const json& fields, std::string& why)
{
    capnp::MallocMessageBuilder message;
    auto root = message.initRoot<capnp::DynamicStruct>(schema);
    std::vector<std::string> errors;
    const bool ok = pub_sub::jsonToCapnp(fields, root, errors);
    for (const auto& error : errors)
    {
        why += error + "; ";
    }
    return ok;
}

void testEverySchemaInTheRegistry()
{
    int built = 0;
    for (const std::string_view name : pub_sub::get_available_schemas())
    {
        const auto schema = pub_sub::get_schema(name);
        if (!schema)
        {
            continue;
        }
        SchemaForm form;
        form.setSchema(schema->asStruct());
        std::string why;
        const std::string label(name);
        std::string problems;
        for (const auto& problem : form.problems())
        {
            problems += problem + "; ";
        }
        expect(form.isValid(), label + ": the default form is valid (" + problems + ")");
        expect(acceptedBySendPath(schema->asStruct(), form.value(), why),
               label + ": the default form's value is accepted by jsonToCapnp (" + why + ")");
        ++built;
    }
    expect(built > 20, "the sweep actually covered the registry");
}

void testFixtureIdentity(capnp::StructSchema schema)
{
    const json full = {
        {"flag", true},
        {"i8", -128},
        {"u8", 255},
        {"i64", INT64_MIN},
        {"u64", UINT64_MAX},
        {"f32", 1.5},
        {"f64", -2.25},
        {"text", "hello"},
        {"bytes", "00ff7a"},
        {"colour", "green"},
        {"inner", {{"label", "x"}, {"count", 65535}}},
        {"colours", {"blue", "red"}},
        {"matrix", {json::array({1, 2}), json::array(), json::array({UINT64_MAX})}},
        {"inners", {{{"label", "a"}, {"count", 1}}, {{"label", "b"}, {"count", 2}}}},
        {"blobs", {"01", "", "dead"}},
        {"small", {0, 7, 255}},
        {"choice", {{"speed", 3.5}}},
        {"pair", {{"a", -5}, {"b", "bee"}}},
    };

    SchemaForm form;
    form.setSchema(schema);

    std::vector<std::string> errors;
    expect(form.setValue(full, errors), "every field shape can be set");
    for (const auto& e : errors)
    {
        std::fprintf(stderr, "  %s\n", e.c_str());
    }
    settle();
    expect(form.value() == full, "and value() gives back exactly what was set");
    if (form.value() != full)
    {
        std::fprintf(stderr, "  in:  %s\n  out: %s\n", full.dump().c_str(), form.value().dump().c_str());
    }
    expect(form.isValid(), "and the form is valid");

    // The widgets show it, under the object names tests and agents address.
    auto* u64 = form.findChild<QLineEdit*>("field:u64");
    expect(u64 != nullptr && u64->text() == "18446744073709551615",
           "a uint64 above 2^53 is shown exactly");
    auto* label = form.findChild<QLineEdit*>("field:inners[1].label");
    expect(label != nullptr && label->text() == "b", "a struct field inside a list is addressable");
    auto* arm = form.findChild<QComboBox*>("field:choice:arm");
    expect(arm != nullptr && arm->currentText() == "speed", "the union shows its chosen arm");
}

void testValidation(capnp::StructSchema schema)
{
    SchemaForm form;
    form.setSchema(schema);
    expect(form.isValid(), "defaults are valid");

    int validity_signals = 0;
    QObject::connect(&form, &SchemaForm::validityChanged, [&]() { ++validity_signals; });

    auto* u8 = form.findChild<QLineEdit*>("field:u8");
    u8->setText("-1");
    expect(!form.isValid(), "a negative UInt8 makes the form invalid");
    expect(!form.problems().empty() && form.problems()[0].starts_with("u8: "),
           "and the problem names the field");
    expect(u8->styleSheet().contains("#C0392B"), "and the field is marked");
    expect(validity_signals == 1, "validityChanged fired once");

    u8->setText("1O");
    expect(!form.problems().empty() &&
               form.problems()[0].find("not an integer") != std::string::npos,
           "text that is not a number says so, rather than 'expected an integer'");

    u8->setText("0x7f");
    expect(form.isValid() && form.value()["u8"] == 127, "hex integers are accepted");
    expect(u8->styleSheet().isEmpty(), "and the mark is cleared");

    auto* bytes = form.findChild<QLineEdit*>("field:bytes");
    bytes->setText("01 ff 7");
    expect(!form.isValid(), "odd-length hex is invalid");
    bytes->setText("01 ff 07");
    expect(form.isValid(), "three whole bytes are valid");
    auto* count = form.findChild<QLabel*>("field:bytes:bytes");
    expect(count != nullptr && count->text() == "3 bytes", "the byte count is shown");
}

void testSetValueIsAllOrNothing(capnp::StructSchema schema)
{
    SchemaForm form;
    form.setSchema(schema);
    const json before = form.value();

    std::vector<std::string> errors;
    expect(!form.setValue({{"text", "changed"}, {"nope", 1}}, errors),
           "an unknown field is refused");
    expect(!errors.empty() && errors[0].find("no such field") != std::string::npos,
           "with a reason");
    expect(form.value() == before, "and the form was left unchanged, good field included");

    errors.clear();
    expect(!form.setValue({{"text", "changed"}, {"u8", 300}}, errors), "an out-of-range value is refused");
    expect(form.value() == before, "and the form was left unchanged");

    errors.clear();
    expect(form.setValue({{"inner", {{"count", 9}}}}, errors), "a partial nested update is accepted");
    expect(form.value()["inner"]["count"] == 9, "and sets the named field");
    expect(form.value()["inner"]["label"] == before["inner"]["label"], "and keeps its sibling");

    form.resetToDefaults();
    expect(form.value() == before, "reset restores the defaults");
}

void testListsAndUnions(capnp::StructSchema schema)
{
    SchemaForm form;
    form.setSchema(schema);

    form.findChild<QPushButton*>("field:small:add")->click();
    form.findChild<QPushButton*>("field:small:add")->click();
    settle();
    expect(form.value()["small"] == json::array({0, 0}), "+ Add appends a default element");

    form.findChild<QLineEdit*>("field:small[1]")->setText("5");
    form.findChild<QPushButton*>("field:small[0]:remove")->click();
    settle();
    expect(form.value()["small"] == json::array({5}),
           "✕ removes that element and the rest move up");
    expect(form.findChild<QLineEdit*>("field:small[0]") != nullptr &&
               form.findChild<QLineEdit*>("field:small[0]")->text() == "5",
           "and the survivor is renamed to its new position");

    form.findChild<QPushButton*>("field:inners:add")->click();
    settle();
    expect(form.value()["inners"].size() == 1 && form.value()["inners"][0].contains("count"),
           "a struct element starts with its schema's fields");

    auto* arm = form.findChild<QComboBox*>("field:choice:arm");
    arm->setCurrentText("name");
    expect(form.value()["choice"] == json({{"name", ""}}), "switching arm changes the union's value");
    arm->setCurrentText("none");
    expect(form.value()["choice"] == json({{"none", nullptr}}), "a Void arm has no payload");
    expect(form.isValid(), "and is valid");
}

void testRealRequestSchemas()
{
    // The Grayhill request is one Data field, and its only documentation is the
    // struct's comment -- which is why doc comments reach the registry at all.
    const auto grayhill = pub_sub::get_schema("GrayhillSetIndicatorsRequest");
    expect(grayhill.has_value(), "the Grayhill request is registered");
    if (grayhill)
    {
        SchemaForm form;
        form.setSchema(grayhill->asStruct());
        std::vector<std::string> errors;
        expect(form.setValue({{"indicators", "ff 00 01"}}, errors), "Grayhill takes hex indicators");
        expect(form.value()["indicators"] == "ff 00 01", "as typed");
    }

    // An enum field offers exactly the schema's enumerants.
    const auto xpr = pub_sub::get_schema("XprSetChannelRequest");
    expect(xpr.has_value(), "the XPR channel request is registered");
    if (xpr)
    {
        SchemaForm form;
        form.setSchema(xpr->asStruct());
        auto* op = form.findChild<QComboBox*>("field:op");
        const auto enumerants = xpr->asStruct().getFieldByName("op").getType().asEnum().getEnumerants();
        expect(op != nullptr && op->count() == static_cast<int>(enumerants.size()),
               "the enum combo lists every enumerant");
    }
}

}  // namespace

int main(int argc, char** argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    spdlog::set_level(spdlog::level::off);

    capnp::SchemaParser parser;
    auto filesystem = kj::newDiskFilesystem();
    std::string directory = FIXTURE_DIR;
    auto fixtures = filesystem->getRoot().openSubdir(kj::Path::parse(directory.substr(1)));
    const capnp::StructSchema fixture =
        parser.parseFromDirectory(*fixtures, kj::Path::parse("capnp_json_fixture.capnp"), nullptr)
            .getNested("Fixture")
            .asStruct();

    testEverySchemaInTheRegistry();
    testFixtureIdentity(fixture);
    testValidation(fixture);
    testSetValueIsAllOrNothing(fixture);
    testListsAndUnions(fixture);
    testRealRequestSchemas();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
