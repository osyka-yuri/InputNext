#include "abi/pe_exports.hpp"

#include "abi_expected_exports.hpp"
#include "filesystem/temporary_directory.hpp"
#include "probe/import_policy.hpp"

#include <catch2/catch_message.hpp>

#include <ios>
#include <span>

#if defined(INPUTNEXT_TEST_UBSAN_EXPORTS) && INPUTNEXT_TEST_UBSAN_EXPORTS
#    include "abi/ubsan_runtime_exports.hpp"

#    include <algorithm>
#    include <cstdint>
#    include <limits>
#    include <utility>
#endif

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>

#ifndef INPUTNEXT_XINPUT_BUNDLE_DIRECTORY
#    error INPUTNEXT_XINPUT_BUNDLE_DIRECTORY must identify the actual production output directory.
#endif

namespace {

using namespace inputnext::test::abi;

[[nodiscard]] std::filesystem::path bundle_directory() {
    return std::filesystem::path{INPUTNEXT_XINPUT_BUNDLE_DIRECTORY};
}

#if defined(INPUTNEXT_TEST_UBSAN_EXPORTS) && INPUTNEXT_TEST_UBSAN_EXPORTS
[[nodiscard]] pe_export_entry* find_export_named(pe_export_snapshot& actual,
    const std::string_view name) {
    const auto entry = std::ranges::find_if(actual.active_entries, [name](const auto& candidate) {
        return std::ranges::find_if(candidate.names, [name](const std::string& candidate_name) {
            return std::string_view{candidate_name.data(), candidate_name.size()} == name;
        }) != candidate.names.end();
    });
    return entry == actual.active_entries.end() ? nullptr : &*entry;
}
#endif

void check_exports(const std::filesystem::path& image,
    const std::span<const expected_export> expected) {
#if defined(INPUTNEXT_TEST_UBSAN_EXPORTS) && INPUTNEXT_TEST_UBSAN_EXPORTS
    const auto error = verify_pe_exports(image, expected, ubsan_runtime_export_names);
#else
    const auto error = verify_pe_exports(image, expected);
#endif
    INFO(error);
    CHECK(error.empty());
}

} // namespace

TEST_CASE("xinput.dll.runtime_import_policy", "[integration][xinput][dll][abi]") {
    using namespace inputnext::xinput_dll_probe;
    for (const bool address_profile : {false, true}) {
        CHECK(runtime_dll_import_allowed("KERNEL32.dll", address_profile));
        CHECK(runtime_symbol_import_allowed("XInputGetState", address_profile));
        CHECK(!(runtime_dll_import_allowed("clang_rt.UBSAN_standalone.dll", address_profile)));
        CHECK(!(runtime_symbol_import_allowed("__ubsan_handle_add_overflow", address_profile)));
        CHECK(!(runtime_symbol_import_allowed(
            "?RegisterUndefinedBehaviorReport@__ubsan@@YAXPEAUUndefinedBehaviorReport@1@@Z",
            address_profile)));
        CHECK(!(runtime_dll_import_allowed("clang_rt.unknown.dll", address_profile)));
    }
    CHECK(!(runtime_dll_import_allowed("CLANG_RT.ASAN_DYNAMIC-x86_64.dll", false)));
    CHECK(!(runtime_symbol_import_allowed("__asan_init", false)));
    CHECK(!(runtime_symbol_import_allowed("__sanitizer_cov_trace_pc_guard", false)));
    CHECK(runtime_dll_import_allowed("clang_rt.asan_dynamic-x86_64.dll", true));
    CHECK(runtime_dll_import_allowed("CLANG_RT.ASAN_DYNAMIC-i386.dll", true));
    CHECK(!(runtime_dll_import_allowed("clang_rt.tsan_asan.dll", true)));
    CHECK(runtime_symbol_import_allowed("__asan_init", true));
    CHECK(runtime_symbol_import_allowed("__sanitizer_cov_trace_pc_guard", true));
}

TEST_CASE("xinput.dll.export_tables_exact", "[integration][xinput][dll][abi]") {
    const auto bundle = bundle_directory();
    check_exports(bundle / L"xinput1_3.dll", xinput13_exports);
    check_exports(bundle / L"xinput1_4.dll", xinput14_exports);
    check_exports(bundle / L"xinput9_1_0.dll", xinput910_exports);
    check_exports(bundle / L"inputnext_runtime.dll", runtime_exports);

#if defined(INPUTNEXT_TEST_UBSAN_EXPORTS) && INPUTNEXT_TEST_UBSAN_EXPORTS
    SECTION("runtime export names are independent of their ordinals") {
        inputnext::test::abi::pe_export_snapshot actual;
        const auto error = inputnext::test::abi::read_pe_exports(bundle / L"xinput1_4.dll", actual);
        INFO(error);
        REQUIRE(error.empty());

        auto* const first =
            find_export_named(actual, inputnext::test::abi::ubsan_runtime_export_names[0U]);
        REQUIRE(first != nullptr);
        inputnext::test::abi::pe_export_entry* second{};
        for (const auto name : inputnext::test::abi::ubsan_runtime_export_names) {
            auto* const candidate = find_export_named(actual, name);
            if (candidate != nullptr && candidate != first) {
                second = candidate;
                break;
            }
        }
        REQUIRE(second != nullptr);
        REQUIRE(first != second);
        std::swap(first->ordinal, second->ordinal);

        CHECK(inputnext::test::abi::verify_public_exports(actual, xinput14_exports).empty());
        CHECK(inputnext::test::abi::verify_runtime_exports(actual,
            xinput14_exports,
            inputnext::test::abi::ubsan_runtime_export_names)
                .empty());
    }

    SECTION("runtime exports may precede the public ordinal range") {
        inputnext::test::abi::pe_export_snapshot actual;
        const auto error = inputnext::test::abi::read_pe_exports(bundle / L"xinput1_4.dll", actual);
        INFO(error);
        REQUIRE(error.empty());

        auto* const runtime_entry =
            find_export_named(actual, inputnext::test::abi::ubsan_runtime_export_names[0U]);
        REQUIRE(runtime_entry != nullptr);
        REQUIRE(actual.ordinal_base > 1U);
        runtime_entry->ordinal = 1U;
        actual.ordinal_base = 1U;
        ++actual.function_slot_count;

        CHECK(inputnext::test::abi::verify_public_exports(actual, xinput14_exports).empty());
        CHECK(inputnext::test::abi::verify_runtime_exports(actual,
            xinput14_exports,
            inputnext::test::abi::ubsan_runtime_export_names)
                .empty());
    }

    SECTION("unknown same-prefix runtime exports are rejected") {
        inputnext::test::abi::pe_export_snapshot actual;
        const auto error = inputnext::test::abi::read_pe_exports(bundle / L"xinput1_4.dll", actual);
        INFO(error);
        REQUIRE(error.empty());

        auto* const injected =
            find_export_named(actual, inputnext::test::abi::ubsan_runtime_export_names[0U]);
        REQUIRE(injected != nullptr);
        REQUIRE(injected->names.size() == 1U);
        injected->names.emplace_back("__ubsan_unrecognized_export");

        CHECK(inputnext::test::abi::verify_public_exports(actual, xinput14_exports).empty());
        CHECK_FALSE(inputnext::test::abi::verify_runtime_exports(actual,
            xinput14_exports,
            inputnext::test::abi::ubsan_runtime_export_names)
                .empty());
    }

    SECTION("unnamed active runtime entries are rejected") {
        inputnext::test::abi::pe_export_snapshot actual;
        const auto error = inputnext::test::abi::read_pe_exports(bundle / L"xinput1_4.dll", actual);
        INFO(error);
        REQUIRE(error.empty());

        auto* const runtime_entry =
            find_export_named(actual, inputnext::test::abi::ubsan_runtime_export_names[0U]);
        REQUIRE(runtime_entry != nullptr);
        const std::uint64_t next_ordinal =
            static_cast<std::uint64_t>(actual.ordinal_base) + actual.function_slot_count;
        REQUIRE(next_ordinal <= std::numeric_limits<std::uint32_t>::max());
        actual.active_entries.push_back({.ordinal = static_cast<std::uint32_t>(next_ordinal),
            .function_rva = runtime_entry->function_rva,
            .names = {}});
        ++actual.function_slot_count;

        CHECK(inputnext::test::abi::verify_public_exports(actual, xinput14_exports).empty());
        CHECK_FALSE(inputnext::test::abi::verify_runtime_exports(actual,
            xinput14_exports,
            inputnext::test::abi::ubsan_runtime_export_names)
                .empty());
    }
#endif
}

TEST_CASE("xinput.dll.export_reader_rejects_foreign_pe_architecture_fields",
    "[integration][xinput][dll][abi]") {
    const auto source_path = bundle_directory() / L"xinput1_4.dll";
    pe_export_snapshot source_snapshot;
    const auto source_error = read_pe_exports(source_path, source_snapshot);
    INFO(source_error);
    REQUIRE(source_error.empty());

    std::ifstream input{source_path, std::ios::binary};
    REQUIRE(input.is_open());
    const std::vector<char> source_image{std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    REQUIRE_FALSE(input.bad());
    REQUIRE_FALSE(source_image.empty());
    REQUIRE(source_image.size() >= sizeof(IMAGE_DOS_HEADER));

    IMAGE_DOS_HEADER dos_header{};
    std::memcpy(&dos_header, source_image.data(), sizeof(dos_header));
    REQUIRE(dos_header.e_magic == IMAGE_DOS_SIGNATURE);
    REQUIRE(dos_header.e_lfanew >= 0);
    const auto nt_offset = static_cast<std::size_t>(dos_header.e_lfanew);
    REQUIRE(nt_offset <= source_image.size());
    REQUIRE(sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) <= source_image.size() - nt_offset);

    DWORD signature{};
    std::memcpy(&signature, source_image.data() + nt_offset, sizeof(signature));
    REQUIRE(signature == IMAGE_NT_SIGNATURE);

    const auto machine_offset = nt_offset + sizeof(DWORD);
    const auto optional_magic_offset = machine_offset + sizeof(IMAGE_FILE_HEADER);
    REQUIRE(sizeof(WORD) <= source_image.size() - machine_offset);
    REQUIRE(sizeof(WORD) <= source_image.size() - optional_magic_offset);

#ifdef _WIN64
    constexpr WORD foreign_machine = IMAGE_FILE_MACHINE_I386;
    constexpr WORD foreign_optional_header_magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
#else
    constexpr WORD foreign_machine = IMAGE_FILE_MACHINE_AMD64;
    constexpr WORD foreign_optional_header_magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
#endif

    const inputnext::test::temporary_directory temporary;
    const auto write_image = [](const std::filesystem::path& path, const std::vector<char>& bytes) {
        std::ofstream output{path, std::ios::binary};
        REQUIRE(output.is_open());
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.flush();
        REQUIRE(output.good());
    };

    auto foreign_machine_image = source_image;
    std::memcpy(foreign_machine_image.data() + machine_offset,
        &foreign_machine,
        sizeof(foreign_machine));
    const auto foreign_machine_path = temporary.path() / "foreign-machine.dll";
    write_image(foreign_machine_path, foreign_machine_image);

    pe_export_snapshot foreign_machine_snapshot;
    const auto foreign_machine_error =
        read_pe_exports(foreign_machine_path, foreign_machine_snapshot);
    INFO(foreign_machine_error);
    CHECK_FALSE(foreign_machine_error.empty());

    auto foreign_optional_header_image = source_image;
    std::memcpy(foreign_optional_header_image.data() + optional_magic_offset,
        &foreign_optional_header_magic,
        sizeof(foreign_optional_header_magic));
    const auto foreign_optional_header_path = temporary.path() / "foreign-optional-header.dll";
    write_image(foreign_optional_header_path, foreign_optional_header_image);

    pe_export_snapshot foreign_optional_header_snapshot;
    const auto foreign_optional_header_error =
        read_pe_exports(foreign_optional_header_path, foreign_optional_header_snapshot);
    INFO(foreign_optional_header_error);
    CHECK_FALSE(foreign_optional_header_error.empty());
}
