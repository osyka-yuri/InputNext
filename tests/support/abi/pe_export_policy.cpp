#include "pe_exports.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace inputnext::test::abi {
namespace {

struct public_export_shape final {
    std::uint32_t first_ordinal{};
    std::uint32_t last_ordinal{};
    std::uint64_t slot_count{};
};

[[nodiscard]] bool includes_public_range(const pe_export_snapshot& actual,
    const public_export_shape& shape) noexcept {
    const std::uint64_t range_start = actual.ordinal_base;
    const std::uint64_t range_end = range_start + actual.function_slot_count;
    return range_start <= shape.first_ordinal && range_end > shape.last_ordinal;
}

[[nodiscard]] std::string failure(const std::string_view message) {
    return std::string{"PE export policy failed: "} + std::string{message};
}

[[nodiscard]] std::string_view validate_snapshot(const pe_export_snapshot& actual) {
    const std::uint64_t end_ordinal =
        static_cast<std::uint64_t>(actual.ordinal_base) + actual.function_slot_count;
    if (end_ordinal > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U) {
        return "snapshot ordinal range overflows";
    }

    for (std::size_t index = 0U; index < actual.active_entries.size(); ++index) {
        const auto& entry = actual.active_entries[index];
        if (entry.function_rva == 0U || entry.ordinal < actual.ordinal_base ||
            static_cast<std::uint64_t>(entry.ordinal) >= end_ordinal) {
            return "active export entry is outside the EAT range";
        }
        if (std::ranges::find(actual.active_entries.begin(),
                actual.active_entries.begin() + static_cast<std::ptrdiff_t>(index),
                entry.ordinal,
                &pe_export_entry::ordinal) !=
            actual.active_entries.begin() + static_cast<std::ptrdiff_t>(index)) {
            return "duplicate active export ordinal";
        }

        for (std::size_t name_index = 0U; name_index < entry.names.size(); ++name_index) {
            const auto& name = entry.names[name_index];
            if (name.empty()) {
                return "export name is empty";
            }
            if (std::ranges::find(entry.names.begin(),
                    entry.names.begin() + static_cast<std::ptrdiff_t>(name_index),
                    name) != entry.names.begin() + static_cast<std::ptrdiff_t>(name_index)) {
                return "duplicate name on one active export entry";
            }
            for (std::size_t other_index = 0U; other_index < index; ++other_index) {
                const auto& previous = actual.active_entries[other_index];
                if (std::ranges::find(previous.names, name) != previous.names.end()) {
                    return "duplicate export name";
                }
            }
        }
    }
    return {};
}

[[nodiscard]] std::string_view get_public_shape(const std::span<const expected_export> expected,
    public_export_shape& result) {
    if (expected.empty()) {
        return "public export contract is empty";
    }

    result.first_ordinal = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        const auto& item = expected[index];
        result.first_ordinal =
            std::min(result.first_ordinal, static_cast<std::uint32_t>(item.ordinal));
        result.last_ordinal =
            std::max(result.last_ordinal, static_cast<std::uint32_t>(item.ordinal));
        if (!item.noname && item.name.empty()) {
            return "named public export has an empty contract name";
        }
        for (std::size_t previous_index = 0U; previous_index < index; ++previous_index) {
            if (expected[previous_index].ordinal == item.ordinal) {
                return "public export contract contains duplicate ordinals";
            }
            if (!item.noname && !expected[previous_index].noname &&
                expected[previous_index].name == item.name) {
                return "public export contract contains duplicate names";
            }
        }
    }
    result.slot_count = static_cast<std::uint64_t>(result.last_ordinal) - result.first_ordinal + 1U;
    return {};
}

[[nodiscard]] const expected_export* find_expected_export(
    const std::span<const expected_export> expected,
    const std::uint32_t ordinal) noexcept {
    const auto match = std::ranges::find_if(expected, [ordinal](const expected_export& item) {
        return static_cast<std::uint32_t>(item.ordinal) == ordinal;
    });
    return match == expected.end() ? nullptr : &*match;
}

[[nodiscard]] const pe_export_entry* find_active_export(const pe_export_snapshot& actual,
    const std::uint32_t ordinal) noexcept {
    const auto match = std::ranges::find(actual.active_entries, ordinal, &pe_export_entry::ordinal);
    return match == actual.active_entries.end() ? nullptr : &*match;
}

[[nodiscard]] bool is_public_ordinal(const std::span<const expected_export> expected,
    const std::uint32_t ordinal) noexcept {
    return find_expected_export(expected, ordinal) != nullptr;
}

[[nodiscard]] std::string_view validate_runtime_contract(
    const std::span<const expected_export> public_exports,
    const std::span<const std::string_view> runtime_names) {
    for (std::size_t index = 0U; index < runtime_names.size(); ++index) {
        if (runtime_names[index].empty()) {
            return "runtime export contract contains an empty name";
        }
        if (std::ranges::find(runtime_names.begin(),
                runtime_names.begin() + static_cast<std::ptrdiff_t>(index),
                runtime_names[index]) !=
            runtime_names.begin() + static_cast<std::ptrdiff_t>(index)) {
            return "runtime export contract contains a duplicate name";
        }
        for (const auto& public_export : public_exports) {
            if (!public_export.noname && public_export.name == runtime_names[index]) {
                return "runtime export contract overlaps a public name";
            }
        }
    }
    return {};
}

} // namespace

std::string verify_public_exports(const pe_export_snapshot& actual,
    const std::span<const expected_export> expected) {
    const auto snapshot_error = validate_snapshot(actual);
    if (!snapshot_error.empty()) {
        return failure(snapshot_error);
    }

    public_export_shape shape{};
    const auto shape_error = get_public_shape(expected, shape);
    if (!shape_error.empty()) {
        return failure(shape_error);
    }
    if (!includes_public_range(actual, shape)) {
        return failure("public ordinals fall outside the EAT range");
    }

    std::size_t public_active_count{};
    for (const auto& entry : actual.active_entries) {
        const auto* const item = find_expected_export(expected, entry.ordinal);
        if (item == nullptr) {
            continue;
        }
        ++public_active_count;
        if (item->noname) {
            if (!entry.names.empty()) {
                return failure(
                    "NONAME ordinal " + std::to_string(item->ordinal) + " has an exported name");
            }
        } else if (entry.names.size() != 1U || std::string_view{entry.names.front().data(),
                                                   entry.names.front().size()} != item->name) {
            return failure("public export name or alias does not match ordinal " +
                           std::to_string(item->ordinal));
        }
    }

    for (const auto& item : expected) {
        if (find_active_export(actual, item.ordinal) == nullptr) {
            return failure("missing public ordinal " + std::to_string(item.ordinal));
        }
    }
    if (public_active_count != expected.size()) {
        return failure("public active export count does not match the contract");
    }
    return {};
}

std::string verify_runtime_exports(const pe_export_snapshot& actual,
    const std::span<const expected_export> public_exports,
    const std::span<const std::string_view> runtime_names) {
    const auto snapshot_error = validate_snapshot(actual);
    if (!snapshot_error.empty()) {
        return failure(snapshot_error);
    }

    public_export_shape public_shape{};
    const auto public_shape_error = get_public_shape(public_exports, public_shape);
    if (!public_shape_error.empty()) {
        return failure(public_shape_error);
    }
    const auto runtime_contract_error = validate_runtime_contract(public_exports, runtime_names);
    if (!runtime_contract_error.empty()) {
        return failure(runtime_contract_error);
    }

    if (!includes_public_range(actual, public_shape)) {
        return failure("public ordinal range falls outside the EAT");
    }
    if (runtime_names.empty() && (actual.ordinal_base != public_shape.first_ordinal ||
                                     actual.function_slot_count != public_shape.slot_count)) {
        return failure("unexpected EAT range outside the exact public interface");
    }

    std::vector<std::size_t> name_counts(runtime_names.size());
    for (const auto& entry : actual.active_entries) {
        if (is_public_ordinal(public_exports, entry.ordinal)) {
            continue;
        }
        if (entry.names.empty()) {
            return failure("active non-public ordinal " + std::to_string(entry.ordinal) +
                           " has no named runtime export");
        }
        for (const auto& name : entry.names) {
            const auto match =
                std::ranges::find(runtime_names, std::string_view{name.data(), name.size()});
            if (match == runtime_names.end()) {
                return failure("unexpected non-public export name " + name);
            }
            const auto index = static_cast<std::size_t>(match - runtime_names.begin());
            ++name_counts[index];
        }
    }

    for (std::size_t index = 0U; index < runtime_names.size(); ++index) {
        if (name_counts[index] != 1U) {
            return failure(
                "runtime export name is missing or repeated: " + std::string{runtime_names[index]});
        }
    }
    return {};
}

std::string verify_pe_exports(const std::filesystem::path& image_path,
    const std::span<const expected_export> public_exports,
    const std::span<const std::string_view> runtime_names) {
    pe_export_snapshot actual;
    auto read_error = read_pe_exports(image_path, actual);
    if (!read_error.empty()) {
        return read_error;
    }

    auto public_error = verify_public_exports(actual, public_exports);
    if (!public_error.empty()) {
        return public_error;
    }
    return verify_runtime_exports(actual, public_exports, runtime_names);
}

} // namespace inputnext::test::abi
