#pragma once

#include "gameinput_probe/manual/sequence.hpp"
#include "gameinput_probe/output/hd_audio/evidence.hpp"
#include "gameinput_probe/output/hd_audio/sequence.hpp"

#include "audio/audio_service.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string_view>

namespace inputnext::gameinput_detail {

class output_session;

enum class hd_audio_step_state : std::uint8_t {
    waiting,
    ready,
    failed,
};

enum class hd_audio_confirmation_prompt : std::uint8_t {
    hd_pulse,
    fallback_pulse,
    classic_pulse,
    output_off,
};

// Owns one diagnostic body context and the evidence for its sequential playback steps.
class hd_audio_step_runner final {
public:
    hd_audio_step_runner(output_session& session, std::ostream& output) noexcept;
    ~hd_audio_step_runner() = default;
    hd_audio_step_runner(const hd_audio_step_runner&) = delete;
    hd_audio_step_runner& operator=(const hd_audio_step_runner&) = delete;
    hd_audio_step_runner(hd_audio_step_runner&&) = delete;
    hd_audio_step_runner& operator=(hd_audio_step_runner&&) = delete;

    [[nodiscard]] std::optional<output::public_context> open(connection_target target);
    [[nodiscard]] bool has_context() const noexcept;
    [[nodiscard]] bool has_receipt() const noexcept;
    [[nodiscard]] bool has_active_step() const noexcept;
    [[nodiscard]] std::string_view failure_reason() const noexcept;
    [[nodiscard]] std::optional<output::backend_progress> current_backend(
        connection_target target) const;
    [[nodiscard]] bool active_backend_is_current(
        const output::backend_progress& backend) const noexcept;

    [[nodiscard]] bool begin(const hd_audio_step& step,
        std::size_t step_number,
        const output::backend_progress& backend,
        const std::optional<audio_detail::audio_target_progress>& audio,
        std::chrono::steady_clock::time_point now);
    [[nodiscard]] hd_audio_step_state observe(const output::backend_progress& backend,
        std::chrono::steady_clock::time_point now);
    void report_progress(std::chrono::steady_clock::time_point now,
        connection_target target,
        const std::optional<audio_detail::audio_target_progress>& audio);
    [[nodiscard]] hd_audio_confirmation_prompt confirmation_prompt() const noexcept;
    [[nodiscard]] std::optional<hd_audio_step_action> complete(
        const manual_step_confirmation& confirmation);
    [[nodiscard]] bool close();
    void cleanup();

private:
    struct active_step final {
        const hd_audio_step* definition{};
        connection_target target{};
        output::backend_handle backend{};
        output::audio_instance audio{};
        output::body_playback_receipt receipt{};
        output::body_snapshot expected_body{};
        output::native_output expected_output{};
        std::optional<output::attempt_id> before_attempt;
        std::uint64_t previous_pcm_buffers{};
        std::uint64_t previous_pcm_frames{};
        std::uint64_t previous_source_frames{};
        std::chrono::steady_clock::time_point evidence_deadline;
        std::chrono::steady_clock::time_point next_report;
        hd_usb_evidence usb_evidence{hd_usb_evidence::pending};
        std::optional<hd_usb_match> match = std::nullopt;
        bool pcm_accepted{};
        bool fallback_observed{};
        bool report_emitted{};
    };

    struct prepared_body_step final {
        output::audio_instance audio{};
        output::body_request request{};
        output::body_playback_update playback{};
    };

    [[nodiscard]] std::optional<prepared_body_step> prepare_body_step(const hd_audio_step& step,
        const output::backend_handle& backend,
        connection_target target,
        const std::optional<audio_detail::audio_target_progress>& audio);
    [[nodiscard]] bool observe_accepted_pcm(std::chrono::steady_clock::time_point now,
        const active_step& current,
        const output::backend_progress& backend);
    void report_pending_progress(std::chrono::steady_clock::time_point now,
        const active_step& current);
    [[nodiscard]] bool wait_for_evidence(std::chrono::steady_clock::time_point now,
        const active_step& current);
    void report_active(hd_usb_evidence evidence, const std::optional<hd_usb_match>& match);
    void fail(std::string_view reason) noexcept;

    output_session& session_;
    std::ostream& output_;
    std::optional<output::public_context> context_;
    std::optional<output::body_playback_receipt> last_receipt_;
    std::optional<active_step> active_;
    std::optional<hd_usb_match> previous_match_;
    std::chrono::steady_clock::time_point next_output_report_;
    std::uint64_t body_sequence_{};
    std::size_t active_step_number_{};
    std::string_view failure_reason_;
};

} // namespace inputnext::gameinput_detail
