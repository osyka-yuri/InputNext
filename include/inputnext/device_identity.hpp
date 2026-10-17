#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace inputnext {

struct runtime_id final {
    std::array<std::byte, 16U> bytes{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return std::ranges::any_of(bytes, [](const std::byte byte) { return byte != std::byte{}; });
    }

    friend constexpr bool operator==(const runtime_id&, const runtime_id&) noexcept = default;
};

struct device_key final {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0U; }

    friend constexpr bool operator==(const device_key&, const device_key&) noexcept = default;
};

struct connection_id final {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0U; }

    friend constexpr bool operator==(const connection_id&, const connection_id&) noexcept = default;
};

struct connection_target final {
    runtime_id runtime{};
    device_key device{};
    connection_id connection{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return runtime.valid() && device.valid() && connection.valid();
    }

    friend constexpr bool operator==(const connection_target&,
        const connection_target&) noexcept = default;
};

} // namespace inputnext
