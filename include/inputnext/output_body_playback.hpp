#pragma once

#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace inputnext::output {

enum class body_pcm_command : std::uint8_t {
    keep,
    stop,
    replace,
};

struct body_playback_update final {
    body_pcm_command command{body_pcm_command::keep};
    prepared_body_pcm replacement;
};

struct body_playback_identity final {
    body_lease_identity lease{};
    std::uint64_t clip_revision{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return lease.valid() && clip_revision != 0U;
    }

    friend constexpr bool operator==(const body_playback_identity&,
        const body_playback_identity&) noexcept = default;
};

struct body_playback_receipt final {
    body_receipt body{};
    std::optional<body_playback_identity> playback;

    friend constexpr bool operator==(const body_playback_receipt&,
        const body_playback_receipt&) noexcept = default;
};

struct body_render_result final {
    std::size_t frames_rendered{};
    std::size_t source_frames_consumed{};
    bool completed{};

    friend constexpr bool operator==(const body_render_result&,
        const body_render_result&) noexcept = default;
};

namespace detail {
struct body_playback_voice_slot;
class body_playback_bank;
} // namespace detail

class body_render_submission;

// The permit borrows a stable authority voice slot and engine busy flag. Keep both owners
// alive until all permits are released and all callers have quiesced.
class body_render_permit final {
public:
    body_render_permit(const body_render_permit&) = delete;
    body_render_permit& operator=(const body_render_permit&) = delete;
    body_render_permit(body_render_permit&& other) noexcept;
    body_render_permit& operator=(body_render_permit&& other) noexcept;
    ~body_render_permit() noexcept;

    [[nodiscard]] body_render_result render(std::span<float> quad_output) && noexcept;
    [[nodiscard]] body_render_submission render_for_submission(
        std::span<float> quad_output) && noexcept;
    [[nodiscard]] bool valid() const noexcept;

private:
    body_render_permit(detail::body_playback_voice_slot& voice,
        std::atomic<bool>& backend_busy,
        std::size_t requested_frames) noexcept;
    void release() noexcept;

    detail::body_playback_voice_slot* voice_{};
    std::atomic<bool>* backend_busy_{};
    std::size_t requested_frames_{};

    friend class detail::body_playback_bank;
};

// Keeps a borrowed voice and backend fence through the native submission that follows mixing.
// The playback bank and executor must outlive this token.
class body_render_submission final {
public:
    body_render_submission() noexcept = default;
    body_render_submission(const body_render_submission&) = delete;
    body_render_submission& operator=(const body_render_submission&) = delete;
    body_render_submission(body_render_submission&& other) noexcept;
    body_render_submission& operator=(body_render_submission&& other) noexcept;
    ~body_render_submission() noexcept;

    [[nodiscard]] body_render_result result() const noexcept { return result_; }

    [[nodiscard]] bool valid() const noexcept;

private:
    body_render_submission(detail::body_playback_voice_slot& voice,
        std::atomic<bool>& backend_busy,
        body_render_result result) noexcept;
    void release() noexcept;

    detail::body_playback_voice_slot* voice_{};
    std::atomic<bool>* backend_busy_{};
    body_render_result result_{};

    friend class body_render_permit;
};

// Clears only complete front-L/R/rear-L/R quads and leaves an incomplete trailing quad untouched.
[[nodiscard]] std::size_t silence_body_block(std::span<float> quad_output,
    std::size_t requested_frames) noexcept;

} // namespace inputnext::output
