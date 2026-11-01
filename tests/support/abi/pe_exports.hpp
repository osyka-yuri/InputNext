#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace inputnext::test::abi {

struct expected_export final {
    std::uint16_t ordinal{};
    std::string_view name;
    bool noname{};
};

struct pe_export_entry final {
    std::uint32_t ordinal{};
    std::uint32_t function_rva{};
    std::vector<std::string> names;
};

struct pe_export_snapshot final {
    std::uint32_t ordinal_base{};
    std::uint32_t function_slot_count{};
    std::vector<pe_export_entry> active_entries;
};

// Reads and validates the PE export structures without applying a product-specific export policy.
[[nodiscard]] std::string read_pe_exports(const std::filesystem::path& image_path,
    pe_export_snapshot& result);

// Checks exact public names, ordinals, NONAME behavior, and public active-entry count.
[[nodiscard]] std::string verify_public_exports(const pe_export_snapshot& actual,
    std::span<const expected_export> expected);

// Checks every active non-public entry against the exact service-name set, independent of ordinal.
[[nodiscard]] std::string verify_runtime_exports(const pe_export_snapshot& actual,
    std::span<const expected_export> public_exports,
    std::span<const std::string_view> runtime_names);

// Combines structural reading with the public and runtime export policies. An empty runtime-name
// set requires that no active export exists outside the exact public interface.
[[nodiscard]] std::string verify_pe_exports(const std::filesystem::path& image_path,
    std::span<const expected_export> public_exports,
    std::span<const std::string_view> runtime_names = {});

} // namespace inputnext::test::abi
