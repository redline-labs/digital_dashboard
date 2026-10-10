#ifndef CONFIG_CODEC_CONFIG_APPLY_LIMITS_H_
#define CONFIG_CODEC_CONFIG_APPLY_LIMITS_H_

#include <concepts>
#include <cstddef>
#include <string>
#include <type_traits>
#include <vector>

#include "reflection/reflection.h"

namespace config_codec
{

// The optional validate() hook described in config_limits.h. Found by ADL, so a
// config declares it as a free function beside the struct.
template <typename Cfg>
concept HasValidate = requires(Cfg& cfg) {
    { validate(cfg) } -> std::same_as<std::vector<std::string>>;
};

// Runs every validate() hook in `cfg` -- its own, and those of every nested
// struct and vector element -- and returns what each one changed, prefixed with
// the path to the struct that said it ("style.widths: motorway was ...").
//
// Children first, so a parent's cross-field rule sees clamped children. A
// parent must therefore NOT call a child's validate() itself: the child would
// be clamped twice and every note reported twice. That rule is what lets a
// shared struct carry its own limits once, instead of every config that
// embeds it repeating them.
template <typename T>
void applyLimits(T& cfg, const std::string& path, std::vector<std::string>& notes)
{
    if constexpr (reflection::is_reflected_struct_v<T>)
    {
        reflection::visit_fields(cfg, [&](std::string_view name, auto& field, std::string_view)
        {
            using Field = std::decay_t<decltype(field)>;
            const std::string field_path = path.empty() ? std::string(name)
                                                        : path + "." + std::string(name);
            if constexpr (reflection::is_reflected_struct_v<Field>)
            {
                applyLimits(field, field_path, notes);
            }
            else if constexpr (reflection::is_std_vector<Field>::value)
            {
                using Elem = typename reflection::is_std_vector<Field>::value_type;
                if constexpr (reflection::is_reflected_struct_v<Elem>)
                {
                    for (std::size_t i = 0; i < field.size(); ++i)
                    {
                        applyLimits(field[i], field_path + "[" + std::to_string(i) + "]", notes);
                    }
                }
            }
        });
    }

    if constexpr (HasValidate<T>)
    {
        for (std::string& note : validate(cfg))
        {
            notes.push_back(path.empty() ? std::move(note) : path + ": " + note);
        }
    }
}

template <typename T>
std::vector<std::string> applyLimits(T& cfg)
{
    std::vector<std::string> notes;
    applyLimits(cfg, std::string(), notes);
    return notes;
}

}  // namespace config_codec

#endif  // CONFIG_CODEC_CONFIG_APPLY_LIMITS_H_
