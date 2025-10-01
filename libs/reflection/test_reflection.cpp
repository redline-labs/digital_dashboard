#include "reflection/reflection.h"

#include <iostream>
#include <string>

// Enum reflection example
REFLECT_ENUM(Color,
    Red,
    Green,
    Blue,
    Orange
)

// Define a struct and expose its fields in one macro
REFLECT_STRUCT(Nested,
    (uint32_t, test1, 43),
    (float, random, 1.0f)
)

REFLECT_STRUCT(Demo,
    (int, id, 0),
    (std::string, name, ""),
    (double, value, 0.0),
    (Nested, nested, Nested{})
)

// Define metadata with friendly names and descriptions for Demo struct.
// Purposely exclude name from metadata to test the fallback to field name.
// Mix entries with and without descriptions to test both formats.
REFLECT_METADATA(Demo,
    (id, "Unique Identifier", "A unique identifier for this demo object"),
    (value, "Numeric Value", "The floating-point value stored in this demo"),
    (nested, "Nested Object")  // No description provided
)

// Example struct without metadata
REFLECT_STRUCT(NoMetadata,
    (int, x, 0),
    (int, y, 0)
)

int main() {
    Demo d;

    std::cout << "Fields and values:\n";
    // Also print names and types via new visitor
    reflection::visit_fields<Demo>(d, [](std::string_view name, auto& /*ref*/, std::string_view type)
    {
        auto friendly_name = reflection::get_friendly_name<Demo>(name);
        auto description = reflection::get_description<Demo>(name);

        std::cout << "* " << name << std::endl;
        std::cout << "   - friendly name = " << friendly_name << std::endl;
        std::cout << "   - description = " << description << std::endl;
        std::cout << "   - type = " << type << std::endl;
        std::cout << std::endl;
    });

    // Enum: names array (constexpr)
    constexpr auto color_names = reflection::enum_traits<Color>::names();
    std::cout << "Color names:" << "\n";
    for (auto sv : color_names) std::cout << " - " << sv << "\n";

    // Enum: to_string
    std::cout << "to_string(Color::Green) = " << reflection::enum_traits<Color>::to_string(Color::Green) << "\n";

    // Enum: from_string
    Color c = reflection::enum_traits<Color>::from_string("Orange");
    std::cout << "from_string('Orange') ok; value index name: " << reflection::enum_traits<Color>::to_string(c) << "\n";

    // Enum: from_string with invalid string
    try
    {
        Color bad_color = reflection::enum_traits<Color>::from_string("invalid");
        std::cout << "bad_color: " << reflection::enum_traits<Color>::to_string(bad_color) << "\n";
    }
    catch (const std::exception& e)
    {
        std::cout << e.what() << "\n";
    }

    // Test the simplified enum_to_string
    std::cout << "enum_to_string(Color::Green) = " << reflection::enum_to_string(Color::Green) << "\n";

    // Test metadata / friendly names
    std::cout << "\n=== Testing Field Metadata ===\n";
    
    // Check if metadata exists
    std::cout << "\nHas metadata checks:\n";
    std::cout << "  Demo has metadata: " << std::boolalpha << reflection::field_metadata_traits<Demo>::has_metadata << "\n";
    std::cout << "  NoMetadata has metadata: " << std::boolalpha << reflection::field_metadata_traits<NoMetadata>::has_metadata << "\n";

    return 0;
}
