#pragma once

#include "program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace inputnext::input {

enum class observation_status : std::uint8_t {
    observed,
    unchanged,
    not_applicable,
    unavailable,
    unsupported,
};

struct touch_contact final {
    std::uint64_t id{};
    double x{};
    double y{};
    bool active{};
};

// One validated observation from a single qualified native reading. The adapter sets
// same_reading only when click, dimensions, and contacts share that reading.
struct touch_observation final {
    observation_status click_status{observation_status::unavailable};
    bool click_pressed{};
    observation_status contacts_status{observation_status::unavailable};
    bool same_reading{};
    bool contacts_complete{};
    double surface_width{};
    double surface_height{};
    std::array<touch_contact, 2U> contacts{};
    std::size_t contact_count{};
    std::uint64_t source_timestamp{};
};

enum class touch_loss_reason : std::uint8_t {
    none,
    initial_click_held,
    missing_click,
    incoherent_reading,
    incomplete_contacts,
    invalid_coordinates,
    unsupported_capability,
};

struct touch_snapshot final {
    touch_source source{touch_source::none};
    bool click_known_pressed{};
    bool awaiting_release{};
    std::uint64_t epoch{};
    std::optional<std::uint64_t> first_accepted_timestamp = std::nullopt;
    touch_loss_reason reason{touch_loss_reason::none};
    bool epoch_exhausted{};

    friend constexpr bool operator==(const touch_snapshot&,
        const touch_snapshot&) noexcept = default;
};

class touch_resolver final {
public:
    [[nodiscard]] touch_snapshot observe(const touch_observation& observation) noexcept;
    [[nodiscard]] touch_snapshot invalidate(touch_loss_reason reason) noexcept;
    [[nodiscard]] touch_snapshot reset() noexcept;
    [[nodiscard]] touch_snapshot snapshot() const noexcept;

private:
    void release() noexcept;
    [[nodiscard]] static touch_source classify(const touch_observation& observation) noexcept;

    touch_snapshot state_{};
    bool has_click_baseline_{};
};

} // namespace inputnext::input
