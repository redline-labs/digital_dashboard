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

int main() {
    Demo d;

    std::cout << "Fields and values:\n";

    // Also print names and types via new visitor
    reflection::visit_fields<Demo>(d, [](std::string_view name, auto& /*ref*/, std::string_view type)
    {
        std::cout << name << ", type = " << type << "\n";
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

    return 0;
}
