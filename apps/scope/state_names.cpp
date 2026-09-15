#include "scope/state_names.h"

#include "pub_sub/capnp_json.h"

#include <cctype>
#include <cmath>

namespace scope
{

QString StateNames::label(double value) const
{
    const auto ordinal = static_cast<long long>(std::llround(value));
    if (ordinal >= 0 && static_cast<std::size_t>(ordinal) < names.size())
    {
        return names[static_cast<std::size_t>(ordinal)];
    }
    return QString::number(value, 'g', 4);
}

StateNames resolveStateNames(pub_sub::schema_type_t schema_type, const std::string& expression)
{
    StateNames out;

    // `name` or `name[<digits>]`, and nothing else. Written out rather than with
    // a regex because <regex> costs more to compile than this does to read.
    std::string field;
    bool indexed = false;
    {
        std::size_t i = 0;
        while (i < expression.size() && std::isspace(static_cast<unsigned char>(expression[i])))
        {
            ++i;
        }
        const std::size_t start = i;
        while (i < expression.size() &&
               (std::isalnum(static_cast<unsigned char>(expression[i])) || expression[i] == '_'))
        {
            ++i;
        }
        field = expression.substr(start, i - start);
        if (field.empty())
        {
            return out;
        }

        if (i < expression.size() && expression[i] == '[')
        {
            indexed = true;
            ++i;
            const std::size_t digits = i;
            while (i < expression.size() && std::isdigit(static_cast<unsigned char>(expression[i])))
            {
                ++i;
            }
            if (i == digits || i >= expression.size() || expression[i] != ']')
            {
                return out;
            }
            ++i;
        }

        while (i < expression.size() && std::isspace(static_cast<unsigned char>(expression[i])))
        {
            ++i;
        }
        if (i != expression.size())
        {
            return out;  // Arithmetic, another variable, anything else.
        }
    }

    const auto schema = pub_sub::get_schema(schema_type);
    if (!schema)
    {
        return out;
    }

    const nlohmann::json described = pub_sub::describeSchema(*schema);
    const auto fields = described.find("fields");
    if (fields == described.end() || !fields->is_object())
    {
        return out;
    }
    const auto entry = fields->find(field);
    if (entry == fields->end())
    {
        return out;
    }

    const std::string type = entry->value("type", std::string{});
    const std::string element = entry->value("element_type", std::string{});
    const std::string effective = (type == "list") ? element : type;

    // A list has to be indexed to be a state; a scalar must not be.
    if ((type == "list") != indexed)
    {
        return out;
    }

    if (effective == "bool")
    {
        out.is_state = true;
        out.names = {QStringLiteral("false"), QStringLiteral("true")};
        return out;
    }

    if (effective != "enum")
    {
        return out;
    }

    out.is_state = true;
    const auto values = entry->find("values");
    if (values != entry->end() && values->is_array())
    {
        // Declaration order, so index N is the name of ordinal N -- which is
        // exactly what the evaluator hands back.
        for (const auto& name : *values)
        {
            out.names.push_back(QString::fromStdString(name.get<std::string>()));
        }
    }
    return out;
}

}  // namespace scope
