#include "inspect/describe.h"

#include "cli/output.h"

#include <algorithm>
#include <vector>

namespace inspect
{

void printSchemaFields(const nlohmann::json& described, const std::string& indent)
{
    if (!described.contains("fields") || !described["fields"].is_object())
    {
        cli::out("{}(no fields)", indent);
        return;
    }

    const nlohmann::json& fields = described["fields"];

    // capnp reports fields in declaration order, which is the order someone
    // reading the .capnp file expects -- so it is preserved rather than sorted.
    // nlohmann::json's default object type sorts keys, so this is already
    // alphabetical by the time we see it; noted so nobody "fixes" it back.
    std::size_t width = 1;
    for (const auto& [name, spec] : fields.items())
    {
        width = std::max(width, name.size());
    }

    for (const auto& [name, spec] : fields.items())
    {
        std::string type = spec.value("type", "?");

        // An enum's whole point is its permitted values; a caller writing a
        // publish or a call needs them, and "enum" alone is useless.
        if (type == "enum" && spec.contains("values") && spec["values"].is_array())
        {
            std::vector<std::string> values;
            for (const auto& value : spec["values"])
            {
                values.push_back(value.get<std::string>());
            }

            std::string joined;
            for (std::size_t i = 0; i < values.size(); ++i)
            {
                joined += (i == 0 ? "" : " | ");
                joined += values[i];
            }
            type = "enum (" + joined + ")";
        }

        cli::out("{}{:<{}}  {}", indent, name, width, type);
    }
}

}  // namespace inspect
