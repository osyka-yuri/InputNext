#pragma once

#include "keystroke_mapping.hpp"
#include "keystroke_tracker.hpp"
#include "output/xinput/rumble.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/history.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/output.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_events.hpp>

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace inputnext::xinput {

struct service::implementation final {
    enum class guide_wait_state : std::uint8_t {
        pending,
        signalled,
        cancelled,
        not_connected,
        ineligible,
        history_reset,
        identity_exhausted,
        closed,
    };

    struct event_qualification final {
        assignment occupant{};
        focus::foreground_epoch focus_epoch{};
        std::uint64_t enable_generation{};

        friend bool operator==(const event_qualification&, const event_qualification&) = default;
    };

    struct guide_wait_record final {
        guide_wait_ticket ticket{};
        assignment occupant{};
        focus::foreground_epoch focus_epoch{};
        std::uint64_t enable_generation{};
        std::optional<history_domain> domain;
        guide_wait_state state{guide_wait_state::pending};
        bool awaiter_active{};
    };

    struct event_position_state final {
        std::optional<event_qualification> qualification;
        std::optional<history_domain> domain;
        std::uint64_t last_sequence{};
        std::uint64_t last_consumer_qpc{};
        std::uint64_t accepted_checkpoint_sequence{};
        std::uint64_t accepted_checkpoint_qpc{};
        bool guide_known{};
        bool guide_pressed{};
        detail::keystroke_tracker tracker;
        std::optional<history_reader_handle> reader;
        bool operation_reserved{};
        bool cleanup_pending{};
        std::optional<guide_wait_record> guide_wait;
    };

    struct staged_event_record final {
        detail::keystroke_sample keys{};
        std::uint64_t sequence{};
        bool baseline{};
        bool guide_known{};
        bool guide_pressed{};
    };

    struct staged_event_batch final {
        std::array<staged_event_record, history_batch_capacity> records{};
        std::size_t count{};
        bool valid{};
    };

    struct position_record final {
        std::optional<assignment> occupant = std::nullopt;
        read_binding reader;
        bool packet_initialized{};
        XINPUT_GAMEPAD canonical_gamepad{};
        std::uint32_t packet_number{};
        std::uint64_t committed_revision{};

        struct desired_rumble_state final {
            ::inputnext::output::rumble_value value{};
            assignment occupant{};
            focus::foreground_epoch epoch{};
            std::uint64_t enable_generation{};
            std::uint64_t request_revision{};
        };

        std::optional<::inputnext::output::internal_context> rumble_context;
        std::optional<desired_rumble_state> desired_rumble;
        std::uint64_t rumble_request_revision{};
        std::uint64_t rumble_logical_version{};
        bool rumble_restore_pending{};
        DWORD rumble_restore_error{ERROR_SUCCESS};
        event_position_state events;
    };

    struct enable_generation_transition final {
        std::uint64_t previous{};
        std::uint64_t next{};
    };

    struct prepared_state_read final {
        assignment occupant;
        read_binding reader;
        focus::snapshot focus_word{};
        bool eligible_at_capture{};
        bool include_guide{};
        XINPUT_GAMEPAD canonical_gamepad{};
        std::uint64_t sample_revision{};
        std::optional<read_pin> sample_pin;
    };

    struct prepared_rumble_request final {
        assignment occupant{};
        focus::foreground_epoch epoch{};
        ::inputnext::output::rumble_value value{};
        std::uint64_t enable_generation{};
        bool enabled{};
        bool known_background{};
    };

    struct event_turn final {
        std::uint32_t user_index{};
        event_source* provider{};
        std::optional<assignment> occupant = std::nullopt;
        focus::snapshot captured_focus{};
        event_qualification qualification{};
        std::optional<history_reader_handle> reader = std::nullopt;
        std::optional<read_pin> lifetime_pin = std::nullopt;
        bool connected{};
        bool eligible{};
        bool operation_reserved{};
        bool cleanup_only{};
        bool needs_open{};
    };

    implementation(focus::observer& focus_observer,
        metadata_source& metadata_provider,
        event_source* event_provider,
        std::uint64_t qpc_frequency,
        std::unique_ptr<::inputnext::output::detail::xinput_rumble> rumble,
        presentation_seed seed);

    [[nodiscard]] static bool same_assignment(const std::optional<assignment>& current,
        const assignment& expected) noexcept {
        return current.has_value() && *current == expected;
    }

    [[nodiscard]] DWORD
    prepare_state_read(DWORD user_index, bool include_guide, prepared_state_read& prepared);
    [[nodiscard]] DWORD commit_state_read(const prepared_state_read& prepared,
        XINPUT_STATE* output);
    [[nodiscard]] DWORD prepare_rumble_request(DWORD user_index,
        const XINPUT_VIBRATION* vibration,
        prepared_rumble_request& prepared);
    [[nodiscard]] DWORD commit_rumble_request(DWORD user_index,
        const prepared_rumble_request& prepared);
    static void clear_rumble_state(position_record& position,
        output::detail::xinput_rumble::transaction* turn,
        DWORD error) noexcept;
    void restore_rumble_state(position_record& position,
        output::detail::xinput_rumble::transaction& turn,
        std::uint64_t enable_generation) const noexcept;
    void transition_rumble_for_enable(position_record& position,
        output::detail::xinput_rumble::transaction* turn,
        bool next_enabled,
        enable_generation_transition generations) const noexcept;
    void maintain_rumble_position(position_record& position,
        output::detail::xinput_rumble::transaction& turn) const noexcept;
    static void maintain_enabled_rumble(position_record& position,
        output::detail::xinput_rumble::transaction& turn,
        const output::detail::xinput_rumble_key& key) noexcept;

    [[nodiscard]] static bool invalidate_event_position(position_record& position,
        guide_wait_state reason) noexcept;
    static void clear_event_stream(event_position_state& events) noexcept;
    void notify_guide_waiters() noexcept;
    void cleanup_idle_event_reader(std::uint32_t user_index) noexcept;
    [[nodiscard]] bool event_qualification_is_current(const event_position_state& events,
        const assignment& occupant,
        const focus::foreground_epoch& epoch) const noexcept;
    [[nodiscard]] static bool stage_batch(const history_batch& batch,
        connection_target target,
        staged_event_batch& staged) noexcept;
    [[nodiscard]] static bool valid_batch_header(const history_batch& batch,
        connection_target target) noexcept;
    [[nodiscard]] static bool valid_batch_shape(const history_batch& batch) noexcept;
    [[nodiscard]] static bool stage_record(const history_record& record,
        std::size_t index,
        bool baseline_batch,
        std::uint64_t previous_sequence,
        staged_event_record& staged) noexcept;
    static void clear_history_state(event_position_state& events) noexcept;
    static void set_guide_terminal(event_position_state& events, guide_wait_state state) noexcept;
    [[nodiscard]] static guide_wait_state history_failure_state(history_failure failure) noexcept;
    [[nodiscard]] static bool same_qualification(const event_position_state& events,
        const assignment& occupant,
        const focus::foreground_epoch& epoch,
        std::uint64_t enable_generation) noexcept;
    [[nodiscard]] static bool batch_matches_history(const event_position_state& events,
        const history_batch& batch,
        const staged_event_batch& staged) noexcept;
    [[nodiscard]] bool apply_batch(event_position_state& events,
        const assignment& occupant,
        const focus::foreground_epoch& epoch,
        std::uint64_t enable_generation,
        const history_batch& batch,
        const staged_event_batch& staged) const noexcept;
    [[nodiscard]] static bool apply_reset_batch(event_position_state& events,
        const assignment& occupant,
        const focus::foreground_epoch& epoch,
        std::uint64_t enable_generation,
        const history_batch& batch,
        const staged_event_batch& staged) noexcept;
    [[nodiscard]] bool apply_event_records(event_position_state& events,
        const assignment& occupant,
        const focus::foreground_epoch& epoch,
        std::uint64_t enable_generation,
        const history_batch& batch,
        const staged_event_batch& staged,
        detail::key_mask& per_turn_repeat_mask) const noexcept;
    [[nodiscard]] bool apply_checkpoint(event_position_state& events,
        const history_batch& batch,
        detail::key_mask& per_turn_repeat_mask) const noexcept;
    [[nodiscard]] bool current_capture_is_qualified(focus::observer::presentation_guard& guard,
        const event_qualification& qualification,
        const focus::snapshot& captured) const noexcept;
    [[nodiscard]] static bool live_binding_matches(position_record& position,
        const assignment& occupant,
        std::optional<read_pin>& pin) noexcept;
    void finish_reserved_operation(std::uint32_t user_index,
        std::optional<history_reader_handle> handle,
        bool close_succeeded,
        bool close_attempted) noexcept;
    void pump_event_turn(std::uint32_t user_index, history_time now) noexcept;
    [[nodiscard]] bool start_event_turn(event_turn& turn) noexcept;
    [[nodiscard]] bool capture_event_turn(focus::observer::presentation_guard& guard,
        position_record& position,
        event_turn& turn,
        bool& notify) noexcept;
    static void reserve_event_operation(position_record& position, event_turn& turn) noexcept;
    [[nodiscard]] bool open_event_reader(event_turn& turn, history_time now) noexcept;
    [[nodiscard]] bool install_event_reader(event_turn& turn,
        history_reader_handle reader) noexcept;
    void handle_event_open_failure(event_turn& turn, history_failure failure) noexcept;
    void commit_event_batch(event_turn& turn, const history_batch& batch) noexcept;
    [[nodiscard]] static bool reject_stale_event_batch(position_record& position,
        bool same_assignment,
        bool owns_qualification,
        bool current,
        bool live,
        bool owns_reader,
        bool& notify) noexcept;
    [[nodiscard]] bool apply_current_event_batch(event_position_state& events,
        const assignment& occupant,
        const event_turn& turn,
        const history_batch& batch,
        const staged_event_batch& staged,
        bool staged_valid,
        bool& notify) const noexcept;
    void close_event_reader(event_turn& turn) noexcept;
    [[nodiscard]] bool pop_eligibility(focus::observer::presentation_guard& guard,
        position_record& position,
        const assignment& occupant,
        std::optional<read_pin>& pin,
        guide_wait_state& failure) const noexcept;
    [[nodiscard]] bool
    try_pop_event(std::uint32_t user_index, XINPUT_KEYSTROKE& output, bool& connected) noexcept;
    [[nodiscard]] bool guide_ticket_is_fresh(focus::observer::presentation_guard& guard,
        position_record& position,
        const guide_wait_record& ticket,
        std::optional<read_pin>& pin,
        guide_wait_state& failure) const noexcept;
    [[nodiscard]] static guide_wait_error guide_wait_error_for_state(
        guide_wait_state state) noexcept;

    focus::observer& focus;
    metadata_source& metadata;
    event_source* event_source_provider{};
    std::unique_ptr<::inputnext::output::detail::xinput_rumble> rumble;
    std::optional<detail::keystroke_repeat_timing> repeat_timing;
    std::array<position_record, 4U> positions{};
    std::uint64_t next_assignment{};
    bool assignment_exhausted{};
    bool enabled{true};
    bool closed{};
    std::uint64_t event_enable_generation{1U};
    bool event_identity_exhausted{};
    std::uint64_t next_guide_ticket_id{1U};
    bool guide_ticket_ids_exhausted{};
    std::optional<std::uint32_t> last_keystroke_position;
    std::mutex guide_wait_mutex;
    std::condition_variable guide_wait_changed;
};

} // namespace inputnext::xinput
