#include "pe_exports.hpp"
#include <filesystem>
#include <ios>

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace inputnext::test::abi {
namespace {

using image_bytes = std::vector<std::uint8_t>;

#ifdef _WIN64
constexpr WORD expected_pe_machine = IMAGE_FILE_MACHINE_AMD64;
constexpr WORD expected_optional_header_magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
using expected_optional_header = IMAGE_OPTIONAL_HEADER64;
constexpr auto expected_architecture_name = "x64";
#else
constexpr WORD expected_pe_machine = IMAGE_FILE_MACHINE_I386;
constexpr WORD expected_optional_header_magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC;
using expected_optional_header = IMAGE_OPTIONAL_HEADER32;
constexpr auto expected_architecture_name = "x86";
#endif

constexpr std::streamoff maximum_image_size = 512LL * 1024LL * 1024LL;
constexpr std::size_t maximum_export_name_length = 4096U;

template <typename T>
[[nodiscard]] bool
read_object(const image_bytes& bytes, const std::size_t offset, T& result) noexcept {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        return false;
    }
    std::memcpy(&result, bytes.data() + offset, sizeof(T));
    return true;
}

[[nodiscard]] bool read_bytes(const image_bytes& bytes,
    const std::size_t offset,
    void* const destination,
    const std::size_t count) noexcept {
    if (offset > bytes.size() || count > bytes.size() - offset) {
        return false;
    }
    std::memcpy(destination, bytes.data() + offset, count);
    return true;
}

struct rva_range final {
    DWORD address{};
    std::size_t byte_count{};
};

struct rva_array final {
    DWORD address{};
    std::size_t element_count{};
};

struct section_view final {
    DWORD virtual_address{};
    DWORD virtual_size{};
    DWORD raw_offset{};
    DWORD raw_size{};
};

struct pe_layout final {
    DWORD size_of_headers{};
    DWORD size_of_image{};
    IMAGE_DATA_DIRECTORY export_directory{};
    std::vector<section_view> sections;
};

struct raw_export_tables final {
    std::vector<DWORD> function_rvas;
    std::vector<DWORD> name_rvas;
    std::vector<WORD> name_ordinals;
};

class pe_image_view final {
public:
    pe_image_view(const image_bytes& bytes, const pe_layout& layout) noexcept
        : bytes_{bytes},
          layout_{layout} {}

    [[nodiscard]] bool read_export_directory(IMAGE_EXPORT_DIRECTORY& result) const noexcept {
        const auto& directory = layout_.export_directory;
        if (directory.VirtualAddress == 0U || directory.Size < static_cast<DWORD>(sizeof(result)) ||
            !is_rva_range_in_image(
                {.address = directory.VirtualAddress, .byte_count = directory.Size})) {
            return false;
        }
        std::size_t offset{};
        if (!rva_to_file_offset({.address = directory.VirtualAddress, .byte_count = sizeof(result)},
                offset)) {
            return false;
        }
        return read_object(bytes_, offset, result) && result.Characteristics == 0U;
    }

    template <typename T>
    [[nodiscard]] bool read_array(const rva_array range, std::vector<T>& result) const {
        if (range.element_count == 0U) {
            result.clear();
            return true;
        }
        if (range.element_count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            return false;
        }
        const std::size_t byte_count = range.element_count * sizeof(T);
        std::size_t offset{};
        if (!rva_to_file_offset({.address = range.address, .byte_count = byte_count}, offset)) {
            return false;
        }
        result.resize(range.element_count);
        return read_bytes(bytes_, offset, result.data(), byte_count);
    }

    [[nodiscard]] bool read_string(const rva_range range, std::string& result) const {
        std::size_t offset{};
        std::size_t available{};
        if (!rva_to_file_span(range.address, offset, available)) {
            return false;
        }
        const std::size_t search_length = std::min(available, range.byte_count);
        const auto* const begin = reinterpret_cast<const char*>(bytes_.data() + offset);
        const auto* const end = begin + search_length;
        const auto* const terminator = std::ranges::find(begin, end, '\0');
        if (terminator == end) {
            return false;
        }
        const auto length = static_cast<std::size_t>(terminator - begin);
        result.assign(begin, length);
        return true;
    }

    [[nodiscard]] bool is_mapped_function_rva(const DWORD rva) const noexcept {
        return is_rva_range_in_image({.address = rva, .byte_count = 1U}) &&
               (rva < layout_.size_of_headers || find_section(rva) != nullptr);
    }

    [[nodiscard]] bool is_forwarder_rva(const DWORD rva) const noexcept {
        const std::uint64_t start = layout_.export_directory.VirtualAddress;
        const std::uint64_t end = start + layout_.export_directory.Size;
        return static_cast<std::uint64_t>(rva) >= start && static_cast<std::uint64_t>(rva) < end;
    }

    [[nodiscard]] bool read_forwarder(const DWORD rva, std::string& result) const {
        const std::uint64_t directory_end =
            static_cast<std::uint64_t>(layout_.export_directory.VirtualAddress) +
            layout_.export_directory.Size;
        const auto remaining = static_cast<std::size_t>(directory_end - rva);
        return read_string(
            {.address = rva, .byte_count = std::min(remaining, maximum_export_name_length + 1U)},
            result);
    }

    [[nodiscard]] bool read_export_name(const DWORD rva, std::string& result) const {
        return read_string({.address = rva, .byte_count = maximum_export_name_length + 1U}, result);
    }

private:
    [[nodiscard]] bool is_rva_range_in_image(const rva_range range) const noexcept {
        const std::uint64_t range_start = range.address;
        if (range.byte_count > std::numeric_limits<std::uint64_t>::max() - range_start) {
            return false;
        }
        const std::uint64_t range_end = range_start + range.byte_count;
        return range_end <= layout_.size_of_image;
    }

    [[nodiscard]] const section_view* find_section(const DWORD rva) const noexcept {
        for (const auto& section : layout_.sections) {
            if (rva < section.virtual_address) {
                continue;
            }
            const std::uint64_t relative =
                static_cast<std::uint64_t>(rva) - section.virtual_address;
            const std::uint64_t mapped_size = std::max(section.virtual_size, section.raw_size);
            if (relative < mapped_size) {
                return &section;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool rva_to_file_span(const DWORD address,
        std::size_t& offset,
        std::size_t& available) const noexcept {
        if (address < layout_.size_of_headers) {
            offset = static_cast<std::size_t>(address);
            available = std::min(static_cast<std::size_t>(layout_.size_of_headers - address),
                bytes_.size() - offset);
            return available != 0U;
        }

        const section_view* const section = find_section(address);
        if (section == nullptr) {
            return false;
        }
        const std::uint64_t relative =
            static_cast<std::uint64_t>(address) - section->virtual_address;
        if (relative >= section->raw_size) {
            return false;
        }
        const std::uint64_t raw_offset = static_cast<std::uint64_t>(section->raw_offset) + relative;
        if (raw_offset > std::numeric_limits<std::size_t>::max()) {
            return false;
        }
        offset = static_cast<std::size_t>(raw_offset);
        if (offset > bytes_.size()) {
            return false;
        }
        const std::uint64_t virtual_remaining =
            std::max(section->virtual_size, section->raw_size) - relative;
        const std::uint64_t raw_remaining = section->raw_size - relative;
        const std::uint64_t file_remaining = bytes_.size() - offset;
        const auto bounded_remaining = std::min({virtual_remaining, raw_remaining, file_remaining});
        available = static_cast<std::size_t>(bounded_remaining);
        return available != 0U;
    }

    [[nodiscard]] bool rva_to_file_offset(const rva_range range,
        std::size_t& offset) const noexcept {
        if (range.byte_count == 0U || !is_rva_range_in_image(range)) {
            return false;
        }
        std::size_t available{};
        return rva_to_file_span(range.address, offset, available) && range.byte_count <= available;
    }

    const image_bytes& bytes_;
    const pe_layout& layout_;
};

[[nodiscard]] bool read_pe_layout(const image_bytes& bytes, pe_layout& result) {
    IMAGE_DOS_HEADER dos_header{};
    if (!read_object(bytes, 0U, dos_header) || dos_header.e_magic != IMAGE_DOS_SIGNATURE ||
        dos_header.e_lfanew < 0) {
        return false;
    }

    const auto nt_offset = static_cast<std::size_t>(dos_header.e_lfanew);
    DWORD signature{};
    IMAGE_FILE_HEADER file_header{};
    if (!read_object(bytes, nt_offset, signature) || signature != IMAGE_NT_SIGNATURE ||
        !read_object(bytes, nt_offset + sizeof(DWORD), file_header) ||
        file_header.Machine != expected_pe_machine ||
        static_cast<std::size_t>(file_header.SizeOfOptionalHeader) <
            sizeof(expected_optional_header)) {
        return false;
    }

    const std::size_t optional_offset = nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    expected_optional_header optional_header{};
    if (!read_object(bytes, optional_offset, optional_header) ||
        optional_header.Magic != expected_optional_header_magic ||
        optional_header.NumberOfRvaAndSizes <= static_cast<DWORD>(IMAGE_DIRECTORY_ENTRY_EXPORT) ||
        optional_header.SizeOfImage == 0U || optional_header.SizeOfHeaders > bytes.size() ||
        optional_header.SizeOfHeaders > optional_header.SizeOfImage) {
        return false;
    }

    const std::size_t section_table_offset =
        optional_offset + static_cast<std::size_t>(file_header.SizeOfOptionalHeader);
    if (section_table_offset > bytes.size() ||
        static_cast<std::size_t>(file_header.NumberOfSections) >
            (bytes.size() - section_table_offset) / sizeof(IMAGE_SECTION_HEADER)) {
        return false;
    }

    pe_layout layout{.size_of_headers = optional_header.SizeOfHeaders,
        .size_of_image = optional_header.SizeOfImage,
        .export_directory = optional_header.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT],
        .sections = {}};
    layout.sections.reserve(file_header.NumberOfSections);
    for (WORD index = 0U; index < file_header.NumberOfSections; ++index) {
        const std::size_t section_offset =
            section_table_offset + (static_cast<std::size_t>(index) * sizeof(IMAGE_SECTION_HEADER));
        IMAGE_SECTION_HEADER header{};
        if (!read_object(bytes, section_offset, header)) {
            return false;
        }

        const auto mapped_size = std::max(header.Misc.VirtualSize, header.SizeOfRawData);
        const std::uint64_t virtual_end =
            static_cast<std::uint64_t>(header.VirtualAddress) + mapped_size;
        const std::uint64_t raw_end =
            static_cast<std::uint64_t>(header.PointerToRawData) + header.SizeOfRawData;
        if (virtual_end > optional_header.SizeOfImage || raw_end > bytes.size()) {
            return false;
        }
        layout.sections.push_back({.virtual_address = header.VirtualAddress,
            .virtual_size = header.Misc.VirtualSize,
            .raw_offset = header.PointerToRawData,
            .raw_size = header.SizeOfRawData});
    }

    result = std::move(layout);
    return true;
}

[[nodiscard]] bool read_export_tables(const pe_image_view& image,
    const IMAGE_EXPORT_DIRECTORY& exports,
    raw_export_tables& result) {
    return image.read_array(
               {.address = exports.AddressOfFunctions, .element_count = exports.NumberOfFunctions},
               result.function_rvas) &&
           image.read_array(
               {.address = exports.AddressOfNames, .element_count = exports.NumberOfNames},
               result.name_rvas) &&
           image.read_array(
               {.address = exports.AddressOfNameOrdinals, .element_count = exports.NumberOfNames},
               result.name_ordinals);
}

[[nodiscard]] bool
valid_function_target(const pe_image_view& image, const DWORD rva, std::string& forwarder) {
    if (!image.is_mapped_function_rva(rva)) {
        return false;
    }
    if (!image.is_forwarder_rva(rva)) {
        return true;
    }
    return image.read_forwarder(rva, forwarder) && !forwarder.empty();
}

[[nodiscard]] bool read_export_names(const pe_image_view& image,
    const IMAGE_EXPORT_DIRECTORY& exports,
    const raw_export_tables& tables,
    pe_export_snapshot& result) {
    std::string previous_name;
    for (std::size_t index = 0U; index < tables.name_rvas.size(); ++index) {
        const std::size_t function_index = tables.name_ordinals[index];
        if (function_index >= tables.function_rvas.size() ||
            tables.function_rvas[function_index] == 0U) {
            return false;
        }

        const std::uint64_t ordinal_value =
            static_cast<std::uint64_t>(exports.Base) + function_index;
        if (ordinal_value > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }

        std::string name;
        if (!image.read_export_name(tables.name_rvas[index], name) || name.empty() ||
            (!previous_name.empty() && name <= previous_name)) {
            return false;
        }
        previous_name = name;
        const auto entry = std::ranges::find(result.active_entries,
            static_cast<std::uint32_t>(ordinal_value),
            &pe_export_entry::ordinal);
        if (entry == result.active_entries.end()) {
            return false;
        }
        entry->names.push_back(std::move(name));
    }
    return true;
}

[[nodiscard]] std::string failure(std::string message) {
    return "PE export check failed: " + std::move(message);
}

} // namespace

std::string read_pe_exports(const std::filesystem::path& image_path, pe_export_snapshot& result) {
    std::ifstream input(image_path, std::ios::binary);
    if (!input) {
        return failure("could not open " + image_path.string());
    }
    input.seekg(0, std::ios::end);
    const auto file_size = static_cast<std::streamoff>(input.tellg());
    if (file_size <= std::streamoff{0} || file_size > maximum_image_size) {
        return failure("invalid file size for " + image_path.string());
    }

    image_bytes bytes(static_cast<std::size_t>(file_size));
    input.seekg(0, std::ios::beg);
    if (!input.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()))) {
        return failure("could not read " + image_path.string());
    }

    pe_layout layout{};
    if (!read_pe_layout(bytes, layout)) {
        return failure(std::string{"invalid or incomplete "} + expected_architecture_name +
                       " PE headers in " + image_path.string());
    }
    const pe_image_view image{bytes, layout};

    IMAGE_EXPORT_DIRECTORY exports{};
    if (!image.read_export_directory(exports)) {
        return failure("missing or invalid export directory in " + image_path.string());
    }
    std::string library_name;
    if (exports.Name == 0U || !image.read_export_name(exports.Name, library_name) ||
        library_name.empty()) {
        return failure("invalid export library name in " + image_path.string());
    }

    const std::uint64_t first_past_ordinal =
        static_cast<std::uint64_t>(exports.Base) + exports.NumberOfFunctions;
    if (first_past_ordinal >
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U) {
        return failure("export ordinal range overflows in " + image_path.string());
    }

    raw_export_tables tables;
    if (!read_export_tables(image, exports, tables)) {
        return failure("invalid export address/name arrays in " + image_path.string());
    }

    pe_export_snapshot snapshot{.ordinal_base = exports.Base,
        .function_slot_count = exports.NumberOfFunctions,
        .active_entries = {}};
    for (std::size_t index = 0U; index < tables.function_rvas.size(); ++index) {
        const DWORD function_rva = tables.function_rvas[index];
        if (function_rva == 0U) {
            continue;
        }

        std::string forwarder;
        if (!valid_function_target(image, function_rva, forwarder)) {
            return failure("export address is outside mapped image data in " + image_path.string());
        }
        snapshot.active_entries.push_back(
            {.ordinal = exports.Base + static_cast<std::uint32_t>(index),
                .function_rva = function_rva,
                .names = {}});
    }

    if (!read_export_names(image, exports, tables, snapshot)) {
        return failure("invalid, duplicate, or unmapped export name in " + image_path.string());
    }

    result = std::move(snapshot);
    return {};
}

} // namespace inputnext::test::abi
