#pragma once

#include "device_identity.hpp"
#include "device_metadata.hpp"
#include "error.hpp"
#include "input_binding.hpp"
#include "xinput_events.hpp"
#include "xinput_types.hpp"
#include <inputnext/history.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace inputnext::focus {
class observer;
}

namespace inputnext::output::detail {
class xinput_rumble;
}

namespace inputnext::xinput::detail {
struct service_test_access;
}

namespace inputnext::xinput {

struct assignment final {
    std::uint32_t user_index{};
    std::uint64_t id{};
    connection_target target{};

    friend bool operator==(const assignment&, const assignment&) noexcept = default;
};

struct presentation_seed final {
    std::uint64_t next_assignment{1U};
};

struct rumble_output_status final {
    bool configured{};
    bool desired_state_cached{};
    bool restore_pending{};
    DWORD last_restore_error{};
    std::uint64_t request_revision{};
    std::uint64_t logical_version{};

    friend bool operator==(const rumble_output_status&,
        const rumble_output_status&) noexcept = default;
};

class metadata_source {
public:
    virtual ~metadata_source() = default;

    metadata_source(const metadata_source&) = delete;
    metadata_source& operator=(const metadata_source&) = delete;
    metadata_source(metadata_source&&) = delete;
    metadata_source& operator=(metadata_source&&) = delete;

    metadata_source() = default;

    // An available nonzero DirectSound GUID identifies a selected, unambiguous endpoint for this
    // target, independently of whether that endpoint's ID is available.
    [[nodiscard]] virtual metadata_result<metadata_snapshot> query(connection_target target) = 0;
};

// Borrows the observer, metadata source, and optional event source, and retains bindings that
// borrow their registry. Close to wake waiters, then join service callers before destroying any
// borrowed owner, the service, or its registry.
class service final {
public:
    service(focus::observer& focus, metadata_source& metadata, presentation_seed seed = {});
    service(focus::observer& focus,
        metadata_source& metadata,
        event_source& events,
        std::uint64_t qpc_frequency,
        presentation_seed seed = {});
    service(focus::observer& focus,
        metadata_source& metadata,
        std::unique_ptr<output::detail::xinput_rumble> rumble,
        presentation_seed seed);
    service(focus::observer& focus,
        metadata_source& metadata,
        event_source& events,
        std::uint64_t qpc_frequency,
        std::unique_ptr<output::detail::xinput_rumble> rumble,
        presentation_seed seed);
    ~service();

    service(const service&) = delete;
    service& operator=(const service&) = delete;
    service(service&&) = delete;
    service& operator=(service&&) = delete;

    // Assignment IDs are scoped to this service instance. Do not pass them to another instance.
    [[nodiscard]] result<assignment> assign(connection_target target, read_binding reader);
    [[nodiscard]] result<void> unassign(assignment expected);
    [[nodiscard]] std::array<std::optional<assignment>, 4U> positions() const;
    void close();
    void enable(BOOL enabled);

    [[nodiscard]] DWORD
    set_state(flavor version, DWORD user_index, const XINPUT_VIBRATION* vibration);
    [[nodiscard]] DWORD power_off_controller(flavor version, DWORD user_index);
    void maintain_output() noexcept;
    [[nodiscard]] std::optional<rumble_output_status> get_rumble_output_status(
        DWORD user_index) const;

    // Retrieves prepared keys only; it does not process history or generate repeats/Guide edges. Call
    // pump_events or composition maintenance explicitly before retrieval when progress is needed.
    [[nodiscard]] DWORD
    get_keystroke(flavor version, DWORD user_index, DWORD reserved, XINPUT_KEYSTROKE* keystroke);
    // Performs event-source work and may wait synchronously in the provider.
    void pump_events(DWORD user_index, history_time now);
    // Tickets reserve one position until a waiter consumes the result or release_guide_wait frees it.
    // A ticket completes on the next committed qualified known released-to-pressed Guide edge;
    // unread earlier physical history may commit after registration.
    [[nodiscard]] guide_wait_result<guide_wait_ticket> begin_guide_wait(DWORD user_index);
    [[nodiscard]] guide_wait_result<void> wait_for_guide_button(guide_wait_ticket ticket);
    [[nodiscard]] guide_wait_result<bool> cancel_guide_wait(guide_wait_ticket ticket);
    [[nodiscard]] guide_wait_result<void> release_guide_wait(guide_wait_ticket ticket);

    // Packet tracking uses the Guide-inclusive translated state; include_guide only projects the
    // response and does not change the packet number.
    [[nodiscard]] DWORD
    get_state(flavor version, DWORD user_index, XINPUT_STATE* state, bool include_guide = false);
    [[nodiscard]] DWORD get_capabilities(flavor version,
        DWORD user_index,
        DWORD flags,
        XINPUT_CAPABILITIES* capabilities);
    [[nodiscard]] DWORD get_capabilities_ex(flavor version,
        DWORD user_index,
        DWORD flags,
        capabilities_ex* capabilities);
    [[nodiscard]] DWORD get_battery_information(flavor version,
        DWORD user_index,
        BYTE device_type,
        XINPUT_BATTERY_INFORMATION* battery);
    [[nodiscard]] DWORD get_audio_device_ids(flavor version,
        DWORD user_index,
        LPWSTR render_id,
        UINT* render_count,
        LPWSTR capture_id,
        UINT* capture_count,
        std::uint32_t* write_flags = nullptr);
    [[nodiscard]] DWORD get_dsound_audio_device_guids(flavor version,
        DWORD user_index,
        GUID* render_guid,
        GUID* capture_guid);

private:
    friend struct detail::service_test_access;

    struct implementation;
    std::unique_ptr<implementation> implementation_;
};

} // namespace inputnext::xinput
