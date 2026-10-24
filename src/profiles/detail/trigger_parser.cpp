#include "trigger_parser.hpp"

#include "profiles/parser.hpp"
#include <inputnext/output.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace inputnext::profiles::detail {
namespace {

using output::trigger_layout;
using output::trigger_mode;
using output::trigger_value;

enum class profile_mode : std::uint8_t {
    off,
    feedback,
    weapon,
    vibration,
    curve,
    slope,
    vibration_curve,
};

constexpr std::array<std::string_view, 9U> known_fields{
    "mode",
    "start",
    "end",
    "strength",
    "frequency",
    "strengths",
    "start_strength",
    "end_strength",
    "amplitudes",
};

[[nodiscard]] bool is_known_field(const std::string_view name) noexcept {
    return std::ranges::find(known_fields, name) != known_fields.end();
}

[[nodiscard]] std::string trigger_prefix(const std::string_view name) {
    return std::string(name) + ": ";
}

[[nodiscard]] bool is_allowed_field(const profile_mode mode, const std::string_view name) noexcept {
    switch (mode) {
    case profile_mode::off:
        return name == "mode";
    case profile_mode::feedback:
        return name == "mode" || name == "start" || name == "strength";
    case profile_mode::weapon:
        return name == "mode" || name == "start" || name == "end" || name == "strength";
    case profile_mode::vibration:
        return name == "mode" || name == "start" || name == "strength" || name == "frequency";
    case profile_mode::curve:
        return name == "mode" || name == "strengths";
    case profile_mode::slope:
        return name == "mode" || name == "start" || name == "end" || name == "start_strength" ||
               name == "end_strength";
    case profile_mode::vibration_curve:
        return name == "mode" || name == "amplitudes" || name == "frequency";
    }
    return false;
}

[[nodiscard]] std::optional<profile_mode> parse_mode(const std::string_view name) noexcept {
    if (name == "off") {
        return profile_mode::off;
    }
    if (name == "feedback") {
        return profile_mode::feedback;
    }
    if (name == "weapon") {
        return profile_mode::weapon;
    }
    if (name == "vibration") {
        return profile_mode::vibration;
    }
    if (name == "curve") {
        return profile_mode::curve;
    }
    if (name == "slope") {
        return profile_mode::slope;
    }
    if (name == "vibration_curve") {
        return profile_mode::vibration_curve;
    }
    return std::nullopt;
}

[[nodiscard]] bool read_number(const toml::node& source,
    const std::string_view field,
    const std::string_view trigger,
    double& result,
    std::vector<diagnostic>& diagnostics) {
    if (const auto value = source.value_exact<std::int64_t>()) {
        result = static_cast<double>(*value);
        return true;
    }
    if (const auto value = source.value_exact<double>()) {
        if (std::isfinite(*value)) {
            result = *value;
            return true;
        }
        diagnostics.push_back(located_diagnostic(source,
            trigger_prefix(trigger) + "'" + std::string(field) + "' must be finite"));
        return false;
    }

    diagnostics.push_back(located_diagnostic(source,
        trigger_prefix(trigger) + "'" + std::string(field) + "' must be a TOML number"));
    return false;
}

[[nodiscard]] std::optional<float> read_normalized(const toml::table& table,
    const std::string_view field,
    const std::string_view trigger,
    const std::optional<float> default_value,
    std::vector<diagnostic>& diagnostics) {
    const toml::node* node = table.get(field);
    if (node == nullptr) {
        if (!default_value) {
            diagnostics.push_back(located_diagnostic(table,
                trigger_prefix(trigger) + "mode requires '" + std::string(field) + "'"));
        }
        return default_value;
    }

    double value{};
    if (!read_number(*node, field, trigger, value, diagnostics)) {
        return std::nullopt;
    }
    if (value < 0.0 || value > 1.0) {
        diagnostics.push_back(located_diagnostic(*node,
            trigger_prefix(trigger) + "'" + std::string(field) + "' must be in the range [0, 1]"));
        return std::nullopt;
    }
    return static_cast<float>(value);
}

[[nodiscard]] std::optional<std::uint8_t> read_frequency(const toml::table& table,
    const std::string_view trigger,
    std::vector<diagnostic>& diagnostics) {
    const toml::node* node = table.get("frequency");
    if (node == nullptr) {
        diagnostics.push_back(located_diagnostic(table,
            trigger_prefix(trigger) + "mode requires 'frequency' in the range [1, 255]"));
        return std::nullopt;
    }

    double value{};
    if (!read_number(*node, "frequency", trigger, value, diagnostics)) {
        return std::nullopt;
    }
    if (std::trunc(value) != value || value < 1.0 || value > 255.0) {
        diagnostics.push_back(located_diagnostic(*node,
            trigger_prefix(trigger) + "'frequency' must be an integer in the range [1, 255]"));
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(value);
}

[[nodiscard]] std::optional<std::array<std::uint8_t, output::trigger_zone_count>> read_levels(
    const toml::table& table,
    const std::string_view field,
    const std::string_view trigger,
    std::vector<diagnostic>& diagnostics) {
    const toml::node* node = table.get(field);
    if (node == nullptr) {
        diagnostics.push_back(located_diagnostic(table,
            trigger_prefix(trigger) + "mode requires '" + std::string(field) +
                "' with exactly 10 integer levels"));
        return std::nullopt;
    }

    const toml::array* values = node->as_array();
    if (values == nullptr) {
        diagnostics.push_back(located_diagnostic(*node,
            trigger_prefix(trigger) + "'" + std::string(field) + "' must be a TOML array"));
        return std::nullopt;
    }
    if (values->size() != output::trigger_zone_count) {
        diagnostics.push_back(located_diagnostic(*node,
            trigger_prefix(trigger) + "'" + std::string(field) +
                "' must contain exactly 10 levels"));
        return std::nullopt;
    }

    std::array<std::uint8_t, output::trigger_zone_count> levels{};
    bool valid = true;
    for (std::size_t index = 0U; index < values->size(); ++index) {
        const toml::node& element = (*values)[index];
        const auto level = element.value_exact<std::int64_t>();
        if (!level) {
            diagnostics.push_back(located_diagnostic(element,
                trigger_prefix(trigger) + "'" + std::string(field) +
                    "' elements must be integers"));
            valid = false;
            continue;
        }
        if (*level < 0 || *level > 8) {
            diagnostics.push_back(located_diagnostic(element,
                trigger_prefix(trigger) + "'" + std::string(field) +
                    "' levels must be in the range [0, 8]"));
            valid = false;
            continue;
        }
        levels[index] = static_cast<std::uint8_t>(*level);
    }
    if (!valid) {
        return std::nullopt;
    }
    return levels;
}

[[nodiscard]] std::uint8_t quantize_strength(const float value) noexcept {
    return static_cast<std::uint8_t>(std::lround(value * 8.0F));
}

[[nodiscard]] std::uint8_t quantize_uniform_position(const float value) noexcept {
    return static_cast<std::uint8_t>(std::lround(static_cast<double>(value) * 9.0));
}

[[nodiscard]] trigger_value off_value() noexcept {
    return trigger_value{};
}

[[nodiscard]] trigger_value uniform_effect(const trigger_mode mode,
    const float start,
    const float end,
    const float strength,
    const float frequency = 0.0F) noexcept {
    return trigger_value{
        .mode = mode,
        .layout = trigger_layout::uniform,
        .start = start,
        .end = end,
        .strength = strength,
        .frequency = frequency,
    };
}

[[nodiscard]] trigger_value zonal_effect(const trigger_mode mode,
    const std::array<std::uint8_t, output::trigger_zone_count>& zones,
    const float frequency = 0.0F) noexcept {
    return trigger_value{
        .mode = mode,
        .layout = trigger_layout::per_zone,
        .start = 0.0F,
        .end = 1.0F,
        .strength = 0.0F,
        .frequency = frequency,
        .zones = zones,
    };
}

[[nodiscard]] bool all_zero(
    const std::array<std::uint8_t, output::trigger_zone_count>& levels) noexcept {
    return std::ranges::all_of(levels, [](const std::uint8_t level) { return level == 0U; });
}

[[nodiscard]] std::array<std::uint8_t, output::trigger_zone_count> build_slope(const float start,
    const float end,
    const std::uint8_t start_level,
    const std::uint8_t end_level) noexcept {
    const auto start_zone =
        std::min(static_cast<std::uint8_t>(std::floor(start * 10.0F)), std::uint8_t{9U});
    const auto end_zone =
        std::min(static_cast<std::uint8_t>(std::floor(end * 10.0F)), std::uint8_t{9U});

    std::array<std::uint8_t, output::trigger_zone_count> levels{};
    for (std::uint8_t zone = 0U; zone < output::trigger_zone_count; ++zone) {
        if (zone < start_zone) {
            levels[zone] = 0U;
        } else if (zone > end_zone) {
            levels[zone] = end_level;
        } else if (start_zone == end_zone) {
            levels[zone] = start_level;
        } else {
            const auto distance = static_cast<float>(zone - start_zone);
            const auto span = static_cast<float>(end_zone - start_zone);
            const auto level_delta =
                static_cast<float>(static_cast<int>(end_level) - static_cast<int>(start_level));
            const auto interpolated =
                static_cast<float>(start_level) + ((distance / span) * level_delta);
            levels[zone] = static_cast<std::uint8_t>(std::clamp(std::lround(interpolated), 0L, 8L));
        }
    }
    return levels;
}

[[nodiscard]] bool append_unexpected_fields(const toml::table& table,
    const profile_mode mode,
    const std::string_view mode_name,
    const std::string_view trigger,
    std::vector<diagnostic>& diagnostics) {
    bool valid = true;
    for (const auto& [key, value] : table) {
        const std::string_view field = key.str();
        if (!is_known_field(field)) {
            diagnostics.push_back(located_diagnostic(value,
                trigger_prefix(trigger) + "unknown field '" + std::string(field) + "'"));
            valid = false;
        } else if (!is_allowed_field(mode, field)) {
            diagnostics.push_back(located_diagnostic(value,
                trigger_prefix(trigger) + "unexpected field '" + std::string(field) +
                    "' for mode '" + std::string(mode_name) + "'"));
            valid = false;
        }
    }
    return valid;
}

void append_unknown_fields(const toml::table& table,
    const std::string_view trigger,
    std::vector<diagnostic>& diagnostics) {
    for (const auto& [key, value] : table) {
        const std::string_view field = key.str();
        if (!is_known_field(field)) {
            diagnostics.push_back(located_diagnostic(value,
                trigger_prefix(trigger) + "unknown field '" + std::string(field) + "'"));
        }
    }
}

[[nodiscard]] std::optional<trigger_value> build_feedback_effect(const toml::table& table,
    const std::string_view trigger,
    std::vector<diagnostic>& diagnostics) {
    const auto start = read_normalized(table, "start", trigger, 0.0F, diagnostics);
    const auto strength = read_normalized(table, "strength", trigger, 0.5F, diagnostics);
    if (!start || !strength) {
        return std::nullopt;
    }
    if (quantize_strength(*strength) == 0U) {
        return off_value();
    }
    return uniform_effect(trigger_mode::feedback, *start, 1.0F, *strength);
}

[[nodiscard]] std::optional<trigger_value> build_weapon_effect(const toml::table& table,
    const std::string_view trigger,
    std::vector<diagnostic>& diagnostics) {
    const auto start = read_normalized(table, "start", trigger, std::nullopt, diagnostics);
    const auto end = read_normalized(table, "end", trigger, std::nullopt, diagnostics);
    const auto strength = read_normalized(table, "strength", trigger, 0.5F, diagnostics);
    if (!start || !end || !strength) {
        return std::nullopt;
    }
    if (*start >= *end) {
        diagnostics.push_back(located_diagnostic(*table.get("end"),
            trigger_prefix(trigger) + "weapon mode requires start to be less than end"));
        return std::nullopt;
    }

    const auto start_zone = quantize_uniform_position(*start);
    const auto end_zone = quantize_uniform_position(*end);
    if (start_zone < 2U || start_zone > 7U) {
        diagnostics.push_back(located_diagnostic(*table.get("start"),
            trigger_prefix(trigger) + "weapon start must quantize to hardware zone [2, 7]"));
        return std::nullopt;
    }
    if (end_zone < 3U || end_zone > 8U || end_zone <= start_zone) {
        diagnostics.push_back(located_diagnostic(*table.get("end"),
            trigger_prefix(trigger) + "weapon end must quantize to a later hardware zone [3, 8]"));
        return std::nullopt;
    }
    if (quantize_strength(*strength) == 0U) {
        diagnostics.push_back(located_diagnostic(*table.get("strength"),
            trigger_prefix(trigger) + "weapon strength must quantize above zero"));
        return std::nullopt;
    }
    return uniform_effect(trigger_mode::weapon, *start, *end, *strength);
}

[[nodiscard]] std::optional<trigger_value> build_vibration_effect(const toml::table& table,
    const std::string_view trigger,
    std::vector<diagnostic>& diagnostics) {
    const auto start = read_normalized(table, "start", trigger, 0.0F, diagnostics);
    const auto strength = read_normalized(table, "strength", trigger, 0.5F, diagnostics);
    const auto frequency = read_frequency(table, trigger, diagnostics);
    if (!start || !strength || !frequency) {
        return std::nullopt;
    }
    if (quantize_strength(*strength) == 0U) {
        return off_value();
    }
    return uniform_effect(trigger_mode::vibration,
        *start,
        1.0F,
        *strength,
        static_cast<float>(*frequency));
}

[[nodiscard]] std::optional<trigger_value> build_curve_effect(const toml::table& table,
    const std::string_view trigger,
    std::vector<diagnostic>& diagnostics) {
    const auto levels = read_levels(table, "strengths", trigger, diagnostics);
    if (!levels) {
        return std::nullopt;
    }
    if (all_zero(*levels)) {
        return off_value();
    }
    return zonal_effect(trigger_mode::feedback, *levels);
}

[[nodiscard]] std::optional<trigger_value> build_slope_effect(const toml::table& table,
    const std::string_view trigger,
    std::vector<diagnostic>& diagnostics) {
    const auto start = read_normalized(table, "start", trigger, std::nullopt, diagnostics);
    const auto end = read_normalized(table, "end", trigger, std::nullopt, diagnostics);
    const auto start_strength =
        read_normalized(table, "start_strength", trigger, std::nullopt, diagnostics);
    const auto end_strength =
        read_normalized(table, "end_strength", trigger, std::nullopt, diagnostics);
    if (!start || !end || !start_strength || !end_strength) {
        return std::nullopt;
    }
    if (*start >= *end) {
        diagnostics.push_back(located_diagnostic(*table.get("end"),
            trigger_prefix(trigger) + "slope mode requires start to be less than end"));
        return std::nullopt;
    }

    const auto levels = build_slope(*start,
        *end,
        quantize_strength(*start_strength),
        quantize_strength(*end_strength));
    if (all_zero(levels)) {
        return off_value();
    }
    return zonal_effect(trigger_mode::feedback, levels);
}

[[nodiscard]] std::optional<trigger_value> build_vibration_curve_effect(const toml::table& table,
    const std::string_view trigger,
    std::vector<diagnostic>& diagnostics) {
    const auto levels = read_levels(table, "amplitudes", trigger, diagnostics);
    const auto frequency = read_frequency(table, trigger, diagnostics);
    if (!levels || !frequency) {
        return std::nullopt;
    }
    if (all_zero(*levels)) {
        return off_value();
    }
    return zonal_effect(trigger_mode::vibration, *levels, static_cast<float>(*frequency));
}

[[nodiscard]] std::optional<trigger_value> build_effect(const toml::table& table,
    const profile_mode mode,
    const std::string_view trigger,
    std::vector<diagnostic>& diagnostics) {
    switch (mode) {
    case profile_mode::off:
        return off_value();
    case profile_mode::feedback:
        return build_feedback_effect(table, trigger, diagnostics);
    case profile_mode::weapon:
        return build_weapon_effect(table, trigger, diagnostics);
    case profile_mode::vibration:
        return build_vibration_effect(table, trigger, diagnostics);
    case profile_mode::curve:
        return build_curve_effect(table, trigger, diagnostics);
    case profile_mode::slope:
        return build_slope_effect(table, trigger, diagnostics);
    case profile_mode::vibration_curve:
        return build_vibration_curve_effect(table, trigger, diagnostics);
    }
    return off_value();
}

} // namespace

diagnostic located_diagnostic(const toml::node& source, std::string message) {
    const auto position = source.source().begin;
    return diagnostic{
        .message = std::move(message),
        .line = position.line,
        .column = position.column,
    };
}

trigger_parse_result parse_trigger(const toml::node& source, const std::string_view name) {
    trigger_parse_result result{};
    const auto* table = source.as_table();
    if (table == nullptr) {
        result.value = off_value();
        result.diagnostics.push_back(
            located_diagnostic(source, trigger_prefix(name) + "must be a TOML table"));
        return result;
    }
    if (table->empty()) {
        return result;
    }

    const toml::node* mode_node = table->get("mode");
    if (mode_node == nullptr) {
        result.diagnostics.push_back(located_diagnostic(*table,
            trigger_prefix(name) + "non-empty trigger tables require 'mode'"));
        append_unknown_fields(*table, name, result.diagnostics);
        result.value = off_value();
        return result;
    }

    const auto mode_name = mode_node->value_exact<std::string>();
    if (!mode_name) {
        result.diagnostics.push_back(
            located_diagnostic(*mode_node, trigger_prefix(name) + "'mode' must be a TOML string"));
        append_unknown_fields(*table, name, result.diagnostics);
        result.value = off_value();
        return result;
    }
    const auto mode = parse_mode(*mode_name);
    if (!mode) {
        result.diagnostics.push_back(located_diagnostic(*mode_node,
            trigger_prefix(name) + "unknown mode '" + *mode_name + "'"));
        append_unknown_fields(*table, name, result.diagnostics);
        result.value = off_value();
        return result;
    }

    const bool valid_fields =
        append_unexpected_fields(*table, *mode, *mode_name, name, result.diagnostics);
    if (!valid_fields) {
        result.value = off_value();
        return result;
    }

    result.value = build_effect(*table, *mode, name, result.diagnostics);
    if (!result.value) {
        result.value = off_value();
    }
    return result;
}

} // namespace inputnext::profiles::detail
