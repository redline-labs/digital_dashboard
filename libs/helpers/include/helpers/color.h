#ifndef HELPERS_COLOR_H
#define HELPERS_COLOR_H

#include <string>
#include <string_view>

namespace helpers
{

/**
 * @brief A type-safe wrapper for color values stored as hex strings
 * 
 * This type provides compile-time type safety for color fields while
 * storing the value as a standard hex string (e.g., "#FF0000" for red).
 * 
 * The editor can detect this type and provide a color picker UI instead
 * of a plain text field.
 */
class Color
{
public:
    // Default constructor - black
    constexpr Color() : value_("#000000") {}
    
    // Construct from string
    constexpr Color(const char* hex) : value_(hex) {}
    Color(const std::string& hex) : value_(hex) {}
    Color(std::string&& hex) : value_(std::move(hex)) {}
    
    // Copy and move
    Color(const Color&) = default;
    Color(Color&&) = default;
    Color& operator=(const Color&) = default;
    Color& operator=(Color&&) = default;
    
    // Assignment from string
    Color& operator=(const std::string& hex) 
    {
        value_ = hex;
        return *this;
    }
    
    // Get the hex string value
    const std::string& value() const { return value_; }
    std::string& value() { return value_; }
    
    // Implicit conversion to string for backwards compatibility
    operator const std::string&() const { return value_; }
    operator std::string&() { return value_; }
    
    // String-like access
    const char* c_str() const { return value_.c_str(); }
    bool empty() const { return value_.empty(); }
    size_t size() const { return value_.size(); }
    
    // Comparison operators
    bool operator==(const Color& other) const { return value_ == other.value_; }
    bool operator!=(const Color& other) const { return value_ != other.value_; }
    bool operator==(const std::string& other) const { return value_ == other; }
    bool operator!=(const std::string& other) const { return value_ != other; }
    
private:
    std::string value_;
};

} // namespace helpers

// YAML serialization support
#include <yaml-cpp/yaml.h>

namespace YAML
{
    template<>
    struct convert<helpers::Color>
    {
        static Node encode(const helpers::Color& rhs)
        {
            Node node;
            node = rhs.value();
            return node;
        }

        static bool decode(const Node& node, helpers::Color& rhs)
        {
            if (!node.IsScalar())
            {
                return false;
            }
            rhs = helpers::Color(node.as<std::string>());
            return true;
        }
    };
}

#endif // HELPERS_COLOR_H

