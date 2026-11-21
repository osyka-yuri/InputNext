#include "parser.hpp"

#include "detail/directinput_parser.hpp"
#include "detail/input_parser.hpp"
#include "detail/located_diagnostic.hpp"
#include "detail/trigger_parser.hpp"
#include <inputnext/output.hpp>

#include <toml++/toml.hpp>

#include <xmmintrin.h>

#include <expected>
#include <iterator>
#include <string>
#include <string_view>

namespace inputnext::profiles {
namespace {

class toml_float_environment final {
public:
    toml_float_environment() noexcept : previous_(_mm_getcsr()) {
        // The default MXCSR preserves subnormals when toml++ multiplies by the sign.
        _mm_setcsr(_MM_MASK_MASK | _MM_ROUND_NEAREST);
    }

    ~toml_float_environment() { _mm_setcsr(previous_); }

    toml_float_environment(const toml_float_environment&) = delete;
    toml_float_environment& operator=(const toml_float_environment&) = delete;
    toml_float_environment(toml_float_environment&&) = delete;
    toml_float_environment& operator=(toml_float_environment&&) = delete;

private:
    unsigned int previous_;
};

} // namespace

parse_result parse_profile(const std::string_view content) {
    if (content.size() > maximum_profile_bytes) {
        return std::unexpected(diagnostic{
            .message = "Profile document exceeds the 64 KiB size limit",
        });
    }

    auto document = [content] {
        const toml_float_environment environment;
        return toml::parse(content);
    }();
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
    if (const auto* input_node = document.table().get("input"); input_node != nullptr) {
        result.settings.input = detail::parse_input(*input_node, result.diagnostics);
    }

    if (const auto* directinput_node = document.table().get("directinput");
        directinput_node != nullptr) {
        result.settings.directinput =
            detail::parse_directinput(*directinput_node, result.diagnostics);
    }

    const auto* effects_node = document.table().get("effects");
    if (effects_node == nullptr) {
        return result;
    }
    const auto* effects = effects_node->as_table();
    if (effects == nullptr) {
        result.settings.effects.left_trigger = output::trigger_value{};
        result.settings.effects.right_trigger = output::trigger_value{};
        result.diagnostics.push_back(
            detail::located_diagnostic(*effects_node, "effects must be a TOML table"));
        return result;
    }

    for (const auto& [key, value] : *effects) {
        const std::string_view name = key.str();
        if (name == "left_trigger" || name == "right_trigger") {
            auto trigger = detail::parse_trigger(value, name);
            if (name == "left_trigger") {
                result.settings.effects.left_trigger = trigger.value;
            } else {
                result.settings.effects.right_trigger = trigger.value;
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
