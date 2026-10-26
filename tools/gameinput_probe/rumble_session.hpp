#pragma once

#include "audio/audio_service.hpp"
#include "gameinput/output/connections/manager.hpp"
#include "gameinput/runtime/output_access.hpp"
#include "output/composition/internal_producer.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/gameinput_runtime.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/xinput.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

namespace inputnext::gameinput_detail {

struct rumble_session_stop_report final {
    bool connection_owner_present{};
    connection_control_report connections{};
    gameinput_stop_report runtime{};
    bool presentation_close_returned{true};
    bool runtime_stopped{};
};

class rumble_session final {
public:
    explicit rumble_session(std::filesystem::path runtime_library);
    ~rumble_session() noexcept;

    rumble_session(const rumble_session&) = delete;
    rumble_session& operator=(const rumble_session&) = delete;
    rumble_session(rumble_session&&) = delete;
    rumble_session& operator=(rumble_session&&) = delete;

    [[nodiscard]] gameinput_result<void> start();
    [[nodiscard]] bool update(maintenance_time now, bool output_started);
    [[nodiscard]] bool focus_is_current_foreground() const noexcept;
    [[nodiscard]] focus::snapshot focus_snapshot() const;
    [[nodiscard]] connection_inventory inventory() const;
    [[nodiscard]] std::array<std::optional<xinput::assignment>, 4U> assignments() const;
    [[nodiscard]] std::optional<xinput::rumble_output_status> output_status(
        std::uint32_t user_index) const;
    [[nodiscard]] gameinput_result<output_description> describe(connection_target target) const;
    [[nodiscard]] metadata_result<bool> qualify_dualsense_usb(connection_target target,
        std::uint64_t expected_catalogue_generation) const;
    [[nodiscard]] metadata_result<metadata_snapshot> query_metadata(connection_target target) const;
    [[nodiscard]] bool update_targeted_output(maintenance_time now, connection_target target);
    [[nodiscard]] output::result<output::detail::internal_producer>
    make_dualsense_diagnostic_producer() &;
    [[nodiscard]] bool start_hd_audio(connection_target target);
    [[nodiscard]] bool retarget_hd_audio(connection_target target) noexcept;
    [[nodiscard]] std::optional<audio_detail::audio_target_progress> hd_audio_progress(
        connection_target target) const noexcept;
    [[nodiscard]] output::result<output::public_context> open_body_context(
        connection_target target);
    [[nodiscard]] output::result<output::prepared_body_pcm>
    prepare_body_pcm(std::span<const float> samples, float gain);
    [[nodiscard]] output::result<output::body_playback_receipt> submit_body_pcm(
        output::public_context context,
        const output::body_request& request,
        const output::body_playback_update& playback);
    [[nodiscard]] output::result<output::close_receipt> close_body_context(
        output::public_context context);
    [[nodiscard]] output::result<void> acknowledge_body_context_close(
        const output::close_receipt& receipt);
    void collect_body_pcm() noexcept;
    [[nodiscard]] std::optional<output::backend_progress> backend_progress(
        connection_target target) const noexcept;
    [[nodiscard]] output::result<output::logical_snapshot> logical_snapshot(
        connection_target target) const;
    [[nodiscard]] xinput::service& presentation() noexcept;
    [[nodiscard]] connection_control_report connection_progress() const noexcept;
    void maintain_output() noexcept;
    void request_drive() noexcept;
    [[nodiscard]] rumble_session_stop_report shutdown() noexcept;

private:
    struct implementation;
    std::unique_ptr<implementation> implementation_;
};

} // namespace inputnext::gameinput_detail
