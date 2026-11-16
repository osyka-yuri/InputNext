#pragma once

#include "profile.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>

namespace inputnext::dinput {

inline constexpr std::size_t max_format_objects = 256U;
inline constexpr DWORD max_format_data_size = 4096U;
inline constexpr DWORD no_profile_object = 0xffffffffU;

struct data_binding final {
    DWORD data_offset{};
    DWORD profile_object_index{no_profile_object};
    object_kind kind{};
};

struct compiled_format final {
    DWORD data_size{};
    std::array<data_binding, max_format_objects> bindings{};
    DWORD binding_count{};
};

static_assert(std::is_trivially_copyable_v<compiled_format>);

[[nodiscard]] HRESULT compile_format(const DIDATAFORMAT& source,
    compiled_format& destination,
    bool force_feedback = false) noexcept;
[[nodiscard]] HRESULT write_state(const compiled_format& format,
    const profile_values& values,
    std::span<std::byte> destination) noexcept;
[[nodiscard]] std::optional<std::size_t>
find_object(DWORD value, DWORD how, const compiled_format* format) noexcept;

} // namespace inputnext::dinput
