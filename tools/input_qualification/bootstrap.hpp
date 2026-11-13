#pragma once

#include "gameinput/input_profile.hpp"
#include "input/touch_resolver.hpp"
#include "report.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace GameInput::v4 {
struct IGameInput;
struct IGameInputDevice;
struct IGameInputReading;
} // namespace GameInput::v4

namespace inputnext::input_qualification {

enum class bootstrap_attempt_result : std::uint8_t {
    not_requested,
    invalid_click_index,
    waiting_for_source_baseline,
    waiting_for_click_held,
    not_due,
    recorded,
    capacity_exhausted,
};

// Diagnostic-only production cursor measurements. This object runs on the control thread and
// never changes the production input profile or its qualification policy.
class bootstrap_measurement final {
public:
    explicit bootstrap_measurement(std::optional<std::uint32_t> click_button);
    ~bootstrap_measurement() = default;

    bootstrap_measurement(const bootstrap_measurement&) = delete;
    bootstrap_measurement& operator=(const bootstrap_measurement&) = delete;
    bootstrap_measurement(bootstrap_measurement&&) = delete;
    bootstrap_measurement& operator=(bootstrap_measurement&&) = delete;

    void configure(std::uint32_t observed_button_count, bool dualsense_usb_raw = false) noexcept;
    void observe_reading(GameInput::v4::IGameInputReading* reading) noexcept;
    [[nodiscard]] bootstrap_attempt_result try_attempt(GameInput::v4::IGameInput* game_input,
        GameInput::v4::IGameInputDevice* device);
    [[nodiscard]] bootstrap_measurement_summary summary(std::string_view status,
        std::uint32_t observed_button_count) const noexcept;

    [[nodiscard]] bool requested() const noexcept;
    [[nodiscard]] bool click_index_valid() const noexcept;
    [[nodiscard]] bool source_baseline_observed() const noexcept;
    [[nodiscard]] std::uint32_t click_button() const noexcept;
    [[nodiscard]] input::touch_snapshot touch_context() const noexcept;
    [[nodiscard]] std::size_t attempt_count() const noexcept;
    [[nodiscard]] bool truncated() const noexcept;

private:
    std::optional<std::uint32_t> click_button_;
    std::optional<gameinput_detail::native_input_profile> profile_;
    input::touch_resolver resolver_{};
    std::vector<bootstrap_attempt_summary> attempts_;
    std::uint64_t qpc_frequency_{};
    std::chrono::steady_clock::time_point next_attempt_;
    bool source_baseline_observed_{};
    bool truncated_{};
};

} // namespace inputnext::input_qualification
