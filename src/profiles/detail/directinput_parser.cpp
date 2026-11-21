#include "directinput_parser.hpp"

#include "located_diagnostic.hpp"
#include "profiles/model.hpp"
#include "profiles/parser.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace inputnext::profiles::detail {
namespace {

[[nodiscard]] bool valid_trigger_threshold(double threshold) noexcept {
    // Compare the encodings so DAZ cannot treat a positive threshold as zero.
    const auto bits = std::bit_cast<std::uint64_t>(threshold);
    return bits != 0U && bits <= std::bit_cast<std::uint64_t>(1.0);
}

} // namespace

directinput_profile parse_directinput(const toml::node& source_node,
    std::vector<diagnostic>& diagnostics) {
    const std::size_t initial_diagnostics = diagnostics.size();
    const auto* table = source_node.as_table();
    if (table == nullptr) {
        diagnostics.push_back(located_diagnostic(source_node, "directinput must be a TOML table"));
        return directinput_profile{};
    }

    directinput_profile result{};
    for (const auto& [key, value] : *table) {
        const std::string_view name = key.str();
        if (name == "digital_triggers") {
            if (const auto boolean = value.value_exact<bool>()) {
                result.digital_triggers = *boolean;
            } else {
                diagnostics.push_back(located_diagnostic(value,
                    "directinput.digital_triggers must be a TOML boolean"));
            }
        } else if (name == "trigger_threshold") {
            if (!value.is_number()) {
                diagnostics.push_back(located_diagnostic(value,
                    "directinput.trigger_threshold must be a TOML number"));
            } else if (const auto threshold = value.value<double>();
                threshold.has_value() && valid_trigger_threshold(*threshold)) {
                result.trigger_threshold = *threshold;
            } else {
                diagnostics.push_back(located_diagnostic(value,
                    "directinput.trigger_threshold must be finite and in (0, 1]"));
            }
        } else {
            diagnostics.push_back(located_diagnostic(value,
                "Unknown directinput field: '" + std::string(name) + "'"));
        }
    }

    return diagnostics.size() == initial_diagnostics ? result : directinput_profile{};
}

} // namespace inputnext::profiles::detail
