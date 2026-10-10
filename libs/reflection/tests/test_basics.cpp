// The reflection primitives everything else walks: field order, labels and the
// fallback for an unlabelled field, nested structs, and enum names both ways.
//
// Every config consumer -- the YAML and JSON codecs, the validator, the
// editor's properties form, the agent interface -- trusts this field list. A
// field visited out of order or a label attached to the wrong field shows up
// there as a plausible wrong form, not as an error.

#include "reflection/reflection.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

REFLECT_ENUM(TestColor,
    Red,
    Green,
    Blue
)

REFLECT_STRUCT(Nested,
    (uint32_t, count, 43),
    (float, ratio, 1.0f)
)

// All three field forms: labelled with a description, unlabelled, and labelled
// without one.
REFLECT_STRUCT(Demo,
    (int, id, 7,
        "Unique Identifier", "A unique identifier for this demo object"),
    (std::string, name, "x"),
    (Nested, nested, Nested{},
        "Nested Object")
)

REFLECT_STRUCT(NoMetadata,
    (int, x, 0),
    (int, y, 0)
)

static_assert(reflection::is_reflected_struct_v<Demo>);
static_assert(reflection::is_reflected_struct_v<Nested>);
static_assert(!reflection::is_reflected_struct_v<int>);
static_assert(reflection::field_metadata_traits<Demo>::has_metadata);
static_assert(!reflection::field_metadata_traits<NoMetadata>::has_metadata);
static_assert(reflection::enum_traits<TestColor>::names().size() == 3);

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

void testFieldOrderAndDefaults()
{
    Demo demo;
    std::vector<std::string> names;
    reflection::visit_fields(demo, [&](std::string_view name, auto&, std::string_view)
    {
        names.emplace_back(name);
    });
    check(names == std::vector<std::string>{"id", "name", "nested"},
          "fields are visited in declaration order");

    check(demo.id == 7 && demo.name == "x" && demo.nested.count == 43u,
          "every field takes its declared default, nested ones included");
}

void testVisitWritesThrough()
{
    Demo demo;
    reflection::visit_fields(demo, [](std::string_view name, auto& ref, std::string_view)
    {
        using Field = std::decay_t<decltype(ref)>;
        if constexpr (std::is_same_v<Field, int>)
        {
            if (name == "id") ref = 99;
        }
    });
    check(demo.id == 99, "visit_fields hands out a reference to the member, not a copy");
}

void testLabels()
{
    check(reflection::get_friendly_name<Demo>("id") == "Unique Identifier",
          "a labelled field reports its label");
    check(reflection::get_description<Demo>("id") ==
              "A unique identifier for this demo object",
          "a labelled field reports its description");
    check(reflection::get_friendly_name<Demo>("nested") == "Nested Object",
          "a field labelled without a description still reports its label");
    check(reflection::get_description<Demo>("nested").empty(),
          "... and an empty description");
    check(!reflection::metadata_covers_all_fields<Demo>(),
          "an unlabelled field is not counted as covered");
}

void testEnums()
{
    using traits = reflection::enum_traits<TestColor>;
    check(traits::to_string(TestColor::Green) == "Green", "enum to string");
    check(traits::from_string("Blue") == TestColor::Blue, "enum from string");
    check(!traits::try_from_string("Purple").has_value(),
          "an unknown enum name is refused, not mapped to the first value");

    bool threw = false;
    try
    {
        (void)traits::from_string("Purple");
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    check(threw, "from_string throws on an unknown name");
}

}  // namespace

int main()
{
    testFieldOrderAndDefaults();
    testVisitWritesThrough();
    testLabels();
    testEnums();

    std::fprintf(stderr, "%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
