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
    
    // True if the string is a hex colour this project understands: "#RGB",
    // "#RRGGBB" or "#RRGGBBAA".
    //
    // Anything else -- a missing '#', a stray letter, a colour name -- lands in
    // QColor as an invalid colour, and an invalid QColor paints as transparent
    // black or, in a stylesheet, makes Qt drop the whole rule. Both are silent,
    // so the point of this is to be able to say so at load time instead.
    static bool isValidFormat(std::string_view text)
    {
        if (text.empty() || text.front() != '#')
        {
            return false;
        }

        const std::string_view digits = text.substr(1);
        if (digits.size() != 3 && digits.size() != 6 && digits.size() != 8)
        {
            return false;
        }

        for (const char c : digits)
        {
            const bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!is_hex)
            {
                return false;
            }
        }
        return true;
    }

    bool isValidFormat() const { return isValidFormat(value_); }

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

