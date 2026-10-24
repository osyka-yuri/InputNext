#include "parser.hpp"

#include "detail/trigger_parser.hpp"
#include <inputnext/output.hpp>

#include <toml++/toml.hpp>

#include <expected>
#include <iterator>
#include <string>
#include <string_view>

namespace inputnext::profiles {

parse_result parse_profile(const std::string_view content) {
    if (content.size() > maximum_profile_bytes) {
        return std::unexpected(diagnostic{
            .message = "Profile document exceeds the 64 KiB size limit",
        });
    }

    auto document = toml::parse(content);
    if (!document) {
        const auto& error = document.error();
        const auto position = error.source().begin;
        return std::unexpected(diagnostic{
            .message = std::string(error.description()),
            .line = position.line,
            .column = position.column,
        });
    }

    parsed_profile result{};
    const auto* effects_node = document.table().get("effects");
    if (effects_node == nullptr) {
        return result;
    }

    const auto* effects = effects_node->as_table();
    if (effects == nullptr) {
        result.settings.left_trigger = output::trigger_value{};
        result.settings.right_trigger = output::trigger_value{};
        result.diagnostics.push_back(
            detail::located_diagnostic(*effects_node, "effects must be a TOML table"));
        return result;
    }

    for (const auto& [key, value] : *effects) {
        const std::string_view name = key.str();
        if (name == "left_trigger" || name == "right_trigger") {
            auto trigger = detail::parse_trigger(value, name);
            if (name == "left_trigger") {
                result.settings.left_trigger = trigger.value;
            } else {
                result.settings.right_trigger = trigger.value;
            }
            result.diagnostics.insert(result.diagnostics.end(),
                std::make_move_iterator(trigger.diagnostics.begin()),
                std::make_move_iterator(trigger.diagnostics.end()));
            continue;
        }

        result.diagnostics.push_back(detail::located_diagnostic(value,
            "Unknown effect name under effects: '" + std::string(name) + "'"));
    }

    return result;
}

} // namespace inputnext::profiles
