#pragma once

#include <GameInput.h>

#include "capture_report.hpp"
#include "input/touch_resolver.hpp"
#include "report.hpp"

#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace inputnext::input_qualification::diagnostic {

struct device_report;

[[nodiscard]] std::uintptr_t canonical_identity(GameInput::v4::IGameInputReading& reading) noexcept;

// Owns the reading chain while GameInput and the selected device are alive.
class native_capture final {
public:
    native_capture() = default;
    ~native_capture() = default;
    native_capture(const native_capture&) = delete;
    native_capture& operator=(const native_capture&) = delete;
    native_capture(native_capture&&) noexcept = default;
    native_capture& operator=(native_capture&&) noexcept = default;

    void advance(GameInput::v4::IGameInput& input,
        GameInput::v4::IGameInputDevice& device,
        const device_report& descriptor);
    void finish(GameInput::v4::IGameInput& input,
        GameInput::v4::IGameInputDevice& device,
        const device_report& descriptor);
    [[nodiscard]] static native_capture read_tail(GameInput::v4::IGameInput& input,
        GameInput::v4::IGameInputDevice& device,
        const device_report& descriptor);

    // Populate views after capture, while the records and their COM owners remain alive.
    [[nodiscard]] native_capture_view view(std::vector<reading_view>& records) const;

private:
    enum class walk_state : std::uint8_t {
        not_started,
        seed_unavailable,
        walking,
        complete,
        capacity_exhausted,
        reference_too_old,
        next_failed,
        tail_seed_unavailable,
        tail_window_end,
        tail_previous_failed,
        tail_budget_exhausted,
    };

    struct release_reading final {
        void operator()(GameInput::v4::IGameInputReading* reading) const noexcept;
    };

    using reading_owner = std::unique_ptr<GameInput::v4::IGameInputReading, release_reading>;

    struct record final {
        reading_sample sample;
        std::uintptr_t identity{};
        reading_owner reading;
    };

    void seed(GameInput::v4::IGameInput& input,
        GameInput::v4::IGameInputDevice& device,
        const device_report& descriptor);
    [[nodiscard]] bool pump(GameInput::v4::IGameInput& input,
        GameInput::v4::IGameInputDevice& device,
        const device_report& descriptor);
    void append(reading_owner reading, const device_report& descriptor, bool reverse);
    [[nodiscard]] std::string_view stop_reason() const noexcept;

    std::vector<record> frames_;
    input::touch_resolver resolver_;
    native_order_diagnostics order_{};
    std::int32_t seed_hresult_{};
    std::int32_t last_hresult_{};
    std::uint64_t no_next_polls_{};
    std::uint64_t transitions_{};
    walk_state state_{walk_state::not_started};
};

} // namespace inputnext::input_qualification::diagnostic
