#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

namespace inputnext::output::detail {

inline constexpr std::size_t body_playback_voice_capacity = 64U;

struct body_playback_slot_index final {
    std::uint8_t value{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return static_cast<std::size_t>(value) < body_playback_voice_capacity;
    }

    friend constexpr bool operator==(body_playback_slot_index,
        body_playback_slot_index) noexcept = default;
};

struct body_playback_slot_selection final {
    body_playback_slot_index slot{};
    connection_target target{};
    // Present when replacement reuses the selected current voice. Installation verifies that
    // it still refers to this identity before extracting its sample owner.
    std::optional<body_playback_identity> displaced;
};

enum class body_playback_admission_error : std::uint8_t {
    invalid_argument,
    busy,
    stale_backend,
};

// Retains extracted sample owners until the caller has released all control gates.
class body_playback_retirement_batch final {
public:
    body_playback_retirement_batch() noexcept = default;
    ~body_playback_retirement_batch() noexcept = default;
    body_playback_retirement_batch(const body_playback_retirement_batch&) = delete;
    body_playback_retirement_batch& operator=(const body_playback_retirement_batch&) = delete;
    body_playback_retirement_batch(body_playback_retirement_batch&&) = delete;
    body_playback_retirement_batch& operator=(body_playback_retirement_batch&&) = delete;

    [[nodiscard]] std::size_t size() const noexcept { return count_; }

private:
    [[nodiscard]] bool has_space() const noexcept { return count_ < entries_.size(); }

    void append(prepared_body_pcm&& samples) noexcept;

    std::array<prepared_body_pcm, body_playback_voice_capacity> entries_{};
    std::size_t count_{};

    friend class body_playback_bank;
};

struct body_playback_voice_slot final {
    body_playback_voice_slot() noexcept = default;
    ~body_playback_voice_slot() noexcept = default;
    body_playback_voice_slot(const body_playback_voice_slot&) = delete;
    body_playback_voice_slot& operator=(const body_playback_voice_slot&) = delete;
    body_playback_voice_slot(body_playback_voice_slot&&) = delete;
    body_playback_voice_slot& operator=(body_playback_voice_slot&&) = delete;

    prepared_body_pcm samples;
    body_playback_identity identity{};
    std::optional<backend_handle> backend;
    bool retired{};
    // Mutated only by a permit while busy is true. Control code reads these after an acquire
    // observation of busy=false while holding the authority gate.
    std::uint64_t cursor{};
    bool completed{};
    std::atomic<bool> busy{false};
};

class body_playback_bank final {
public:
    body_playback_bank() noexcept = default;
    ~body_playback_bank() noexcept = default;
    body_playback_bank(const body_playback_bank&) = delete;
    body_playback_bank& operator=(const body_playback_bank&) = delete;
    body_playback_bank(body_playback_bank&&) = delete;
    body_playback_bank& operator=(body_playback_bank&&) = delete;

    // Every control method below is called while the single owning authority gate is held.
    // The bank owns no lock, lease authority, counter, or capacity beyond its fixed slots.
    [[nodiscard]] std::optional<body_playback_slot_index> find_current(
        body_lease_identity identity) const noexcept;
    [[nodiscard]] std::optional<body_playback_slot_selection> select_install_slot(
        connection_target target,
        std::optional<body_playback_slot_index> current) const noexcept;
    [[nodiscard]] bool install(body_playback_slot_selection selection,
        std::optional<body_playback_identity> previous,
        body_playback_identity identity,
        prepared_body_pcm&& replacement,
        body_playback_retirement_batch& displaced) noexcept;

    [[nodiscard]] bool stop(body_lease_identity identity) noexcept;
    [[nodiscard]] std::size_t retire(body_playback_identity identity) noexcept;
    [[nodiscard]] std::size_t retire_lease(body_lease_identity identity) noexcept;
    [[nodiscard]] std::size_t retire_started_lease(body_lease_identity identity) noexcept;
    [[nodiscard]] std::size_t retire_audio(audio_instance identity) noexcept;
    [[nodiscard]] std::size_t retire_backend(backend_handle identity) noexcept;

    [[nodiscard]] std::expected<std::optional<body_render_permit>, body_playback_admission_error>
    try_claim(body_playback_identity expected,
        backend_handle backend,
        std::atomic<bool>* engine_busy,
        std::size_t requested_frames) noexcept;

    // Completed and retired slots are moved to the caller's fixed batch. The caller must let
    // that batch release its sample owners only after dropping all authority/control gates.
    [[nodiscard]] std::size_t collect_nonbusy(body_playback_retirement_batch& destination) noexcept;

private:
    [[nodiscard]] body_playback_voice_slot& at(body_playback_slot_index index) noexcept;
    [[nodiscard]] const body_playback_voice_slot& at(body_playback_slot_index index) const noexcept;
    [[nodiscard]] static bool same_target(const body_playback_voice_slot& slot,
        connection_target target) noexcept;

    std::array<body_playback_voice_slot, body_playback_voice_capacity> slots_{};

    friend class ::inputnext::output::body_render_permit;
};

} // namespace inputnext::output::detail
