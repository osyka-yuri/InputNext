#include "input_parser.hpp"

#include "input/program.hpp"
#include "input/remappable_buttons.hpp"
#include "input_control_names.hpp"
#include "profiles/parser.hpp"
#include "trigger_parser.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <string>
#include <string_view>
#include <vector>

namespace inputnext::profiles::detail {
namespace {

using input::is_trigger_source;
using input::program;
using input::program_builder;
using input::remappable_buttons;
using input::route;
using input::source;
using input::target_kind;

using seen_sources = std::array<bool, static_cast<std::size_t>(source::count)>;

[[nodiscard]] std::optional<route> parse_target_name(const std::string_view name,
    const control_layout& layout) noexcept {
    if (name == "disabled") {
        return route{.kind = target_kind::disabled};
    }
    const auto control = resolve_control(name, layout);
    if (!control.has_value()) {
        return std::nullopt;
    }
    if (is_trigger_source(*control)) {
        return route{.kind = target_kind::trigger,
            .right_trigger = *control == source::right_trigger};
    }
    const auto index = static_cast<std::size_t>(*control);
    if (index < remappable_buttons.size()) {
        return route{.kind = target_kind::button, .button = remappable_buttons[index].button};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<double> parse_threshold(const toml::node& node,
    std::vector<diagnostic>& diagnostics) {
    double value{};
    if (const auto integer = node.value_exact<std::int64_t>()) {
        value = static_cast<double>(*integer);
    } else if (const auto floating = node.value_exact<double>()) {
        value = *floating;
    } else {
        diagnostics.push_back(
            located_diagnostic(node, "Input remap threshold must be a TOML number"));
        return std::nullopt;
    }
    if (!std::isfinite(value) || value <= 0.0 || value > 1.0) {
        diagnostics.push_back(
            located_diagnostic(node, "Input remap threshold must be finite and in (0, 1]"));
        return std::nullopt;
    }
    return value;
}

struct rule_fields final {
    const toml::node* from{};
    const toml::node* to{};
    const toml::node* threshold{};
};

[[nodiscard]] std::optional<rule_fields> read_rule_fields(const toml::table& table,
    std::vector<diagnostic>& diagnostics) {
    rule_fields fields{};
    bool valid = true;
    for (const auto& [key, value] : table) {
        const std::string_view name = key.str();
        if (name == "from") {
            fields.from = &value;
        } else if (name == "to") {
            fields.to = &value;
        } else if (name == "threshold") {
            fields.threshold = &value;
        } else {
            diagnostics.push_back(located_diagnostic(value,
                "Unknown input remap rule field: '" + std::string(name) + "'"));
            valid = false;
        }
    }
    return valid ? std::optional{fields} : std::nullopt;
}

[[nodiscard]] std::optional<route> parse_target_node(const toml::node& node,
    const control_layout& layout,
    std::vector<diagnostic>& diagnostics) {
    const auto target_name = node.value_exact<std::string>();
    if (!target_name.has_value()) {
        diagnostics.push_back(
            located_diagnostic(node, "Input remap destination must be a TOML string"));
        return std::nullopt;
    }
    auto target = parse_target_name(*target_name, layout);
    if (!target.has_value()) {
        diagnostics.push_back(
            located_diagnostic(node, "Unknown input remap destination: '" + *target_name + "'"));
    }
    return target;
}

[[nodiscard]] bool apply_threshold(const toml::node* threshold_node,
    const source input_source,
    route& target,
    std::vector<diagnostic>& diagnostics) {
    if (threshold_node == nullptr) {
        return true;
    }
    if (!is_trigger_source(input_source) || target.kind != target_kind::button) {
        diagnostics.push_back(located_diagnostic(*threshold_node,
            "Threshold is allowed only for an analog trigger mapped to a button"));
        return false;
    }
    const auto threshold = parse_threshold(*threshold_node, diagnostics);
    if (!threshold.has_value()) {
        return false;
    }
    target.threshold = *threshold;
    return true;
}

[[nodiscard]] std::optional<route> parse_route(const rule_fields& fields,
    const source input_source,
    const control_layout& layout,
    const toml::node& context,
    std::vector<diagnostic>& diagnostics) {
    if (fields.to == nullptr) {
        diagnostics.push_back(located_diagnostic(context, "Input remap rule requires 'to'"));
        return std::nullopt;
    }
    auto target = parse_target_node(*fields.to, layout, diagnostics);
    if (!target.has_value()) {
        return std::nullopt;
    }
    if (!apply_threshold(fields.threshold, input_source, *target, diagnostics)) {
        return std::nullopt;
    }
    return target;
}

[[nodiscard]] bool register_source(const source input_source,
    const std::string_view name,
    const toml::node& location,
    seen_sources& seen,
    std::vector<diagnostic>& diagnostics) {
    const auto index = static_cast<std::size_t>(input_source);
    if (seen[index]) {
        diagnostics.push_back(located_diagnostic(location,
            "Duplicate physical input source: '" + std::string(name) + "'"));
        return false;
    }
    seen[index] = true;
    return true;
}

void parse_rule(const toml::node& node,
    const control_layout& layout,
    program_builder& builder,
    seen_sources& seen,
    std::vector<diagnostic>& diagnostics) {
    const auto* table = node.as_table();
    if (table == nullptr) {
        diagnostics.push_back(
            located_diagnostic(node, "Each input.remap entry must be a TOML table"));
        return;
    }

    const auto fields = read_rule_fields(*table, diagnostics);
    if (!fields.has_value()) {
        return;
    }
    if (fields->from == nullptr) {
        diagnostics.push_back(located_diagnostic(node, "Input remap rule requires 'from'"));
        return;
    }
    const auto source_name = fields->from->value_exact<std::string>();
    if (!source_name.has_value()) {
        diagnostics.push_back(
            located_diagnostic(*fields->from, "Input remap source must be a TOML string"));
        return;
    }
    const auto input_source = resolve_control(*source_name, layout);
    if (!input_source.has_value()) {
        diagnostics.push_back(located_diagnostic(*fields->from,
            "Unknown input remap source: '" + *source_name + "'"));
        return;
    }
    if (!register_source(*input_source, *source_name, *fields->from, seen, diagnostics)) {
        return;
    }
    const auto target = parse_route(*fields, *input_source, layout, node, diagnostics);
    if (target.has_value() && !builder.set(*input_source, *target)) {
        diagnostics.push_back(located_diagnostic(node, "Invalid input remap assignment"));
    }
}

void parse_rules(const toml::node& node,
    const control_layout& layout,
    program_builder& builder,
    seen_sources& seen,
    std::vector<diagnostic>& diagnostics) {
    const auto* array = node.as_array();
    if (array == nullptr) {
        diagnostics.push_back(located_diagnostic(node, "input.remap must be an array of rules"));
        return;
    }
    for (const auto& rule : *array) {
        parse_rule(rule, layout, builder, seen, diagnostics);
    }
}

[[nodiscard]] const control_layout* parse_layout(const toml::node& node,
    std::vector<diagnostic>& diagnostics) {
    const auto name = node.value_exact<std::string>();
    if (!name.has_value()) {
        diagnostics.push_back(located_diagnostic(node, "input.layout must be a TOML string"));
        return nullptr;
    }
    const auto* const layout = find_layout(*name);
    if (layout == nullptr) {
        diagnostics.push_back(located_diagnostic(node, "Unknown input layout: '" + *name + "'"));
    }
    return layout;
}

} // namespace

program parse_input(const toml::node& source_node, std::vector<diagnostic>& diagnostics) {
    const std::size_t initial_diagnostics = diagnostics.size();
    const auto* table = source_node.as_table();
    if (table == nullptr) {
        diagnostics.push_back(located_diagnostic(source_node, "input must be a TOML table"));
        return program::defaults();
    }

    const control_layout* layout = &universal_layout;
    const toml::node* remap_node = nullptr;
    for (const auto& [key, value] : *table) {
        const std::string_view name = key.str();
        if (name == "layout") {
            layout = parse_layout(value, diagnostics);
        } else if (name == "remap") {
            remap_node = &value;
        } else {
            diagnostics.push_back(
                located_diagnostic(value, "Unknown input field: '" + std::string(name) + "'"));
        }
    }

    program_builder builder{program::defaults()};
    seen_sources seen{};
    if (remap_node != nullptr && layout != nullptr) {
        parse_rules(*remap_node, *layout, builder, seen, diagnostics);
    }

    const auto compiled = builder.build();
    if (!compiled.has_value()) {
        diagnostics.push_back(located_diagnostic(source_node, "Invalid input remap program"));
    }
    if (diagnostics.size() != initial_diagnostics || !compiled.has_value()) {
        return program::defaults();
    }
    return *compiled;
}

} // namespace inputnext::profiles::detail
