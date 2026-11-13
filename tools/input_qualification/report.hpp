#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>

namespace inputnext::input_qualification {

inline constexpr std::size_t bootstrap_attempt_capacity = 1024U;

[[nodiscard]] std::string escape_json_string(std::string_view value);

struct dualsense_usb_touch_point final {
    std::uint64_t id{};
    std::int32_t x{};
    std::int32_t y{};
};

enum class dualsense_usb_touch_side : std::uint8_t { none, left, right, fallback };

struct dualsense_usb_touch_record final {
    bool attempted{};
    bool click_state_available{};
    bool click_pressed{};
    bool available{};
    bool side_evaluated{};
    dualsense_usb_touch_side resolved_side{dualsense_usb_touch_side::none};
    std::uint32_t contact_count{};
    std::array<dualsense_usb_touch_point, 2U> contacts{};
};

inline constexpr std::size_t reading_surface_capacity = 8U;
inline constexpr std::size_t reading_contact_capacity = 2U;

struct reading_contact_payload final {
    std::uint64_t id{};
    std::int32_t x{};
    std::int32_t y{};
    std::uint32_t pad_x{};
    std::uint32_t pad_y{};
};

struct reading_sample final {
    std::uint64_t timestamp{};
    std::uint32_t input_kind{};
    bool gamepad_available{};
    std::uint32_t gamepad_buttons{};
    float left_trigger{};
    float right_trigger{};
    float left_thumbstick_x{};
    float left_thumbstick_y{};
    float right_thumbstick_x{};
    float right_thumbstick_y{};
    std::uint32_t reading_button_count{};
    std::uint32_t returned_button_count{};
    std::uint64_t button_bits{};
    std::uint32_t surface_count{};
    std::array<std::uint32_t, reading_surface_capacity> reported_contact_counts{};
    std::array<std::uint32_t, reading_surface_capacity> returned_contact_counts{};
    std::array<std::array<reading_contact_payload, reading_contact_capacity>,
        reading_surface_capacity>
        contacts{};
    bool button_count_truncated{};
    bool surface_count_truncated{};
    bool contact_count_truncated{};
    dualsense_usb_touch_record raw_touch{};
};

struct native_capture_summary final {
    std::string_view status;
    std::uint32_t filter_input_kind{};
    std::size_t records{};
    std::size_t record_capacity{};
    bool truncated{};
    std::uint64_t identity_failures{};
    std::uint64_t transition_count{};
    std::string_view stop_reason;
    std::int32_t seed_hresult{};
    std::int32_t last_hresult{};
    std::uint64_t no_next_polls{};
    std::string_view next_stop_reason;
    std::uint64_t next_transitions{};
    std::uint64_t next_identity_matches{};
    std::uint64_t callback_native_identity_matches{};
    std::uint64_t native_records_seen_in_callback{};
    std::uint64_t payload_matches{};
    std::uint64_t payload_mismatches{};
    std::uint64_t gesture_bearing_identity_matches{};
    std::uint64_t gesture_bearing_payload_matches{};
    std::uint64_t same_timestamp_distinct_readings{};
    std::uint64_t timestamp_order_violations{};
};

struct bootstrap_attempt_summary final {
    bool timing_available{};
    bool factory_measured{};
    bool start_measured{};
    bool destruction_measured{};
    bool total_measured{};
    std::uint64_t factory_ticks{};
    std::uint64_t start_ticks{};
    std::uint64_t destruction_ticks{};
    std::uint64_t total_ticks{};
    std::uint32_t transitions{};
    bool succeeded{};
    bool fallback{};
    std::int32_t native_code{};
    std::string_view reason;
    std::string_view touch_source;
    std::string_view touch_reason;
    bool context_baseline_available{};
    bool context_click_pressed{};
    bool context_awaiting_release{};
    bool context_has_first_timestamp{};
    std::uint64_t context_epoch{};
    std::uint64_t context_first_timestamp{};
};

struct bootstrap_measurement_summary final {
    bool requested{};
    bool click_index_valid{};
    bool raw_usb_coordinates{};
    bool source_baseline_observed{};
    std::uint32_t click_button{};
    std::uint32_t observed_button_count{};
    std::uint64_t qpc_frequency{};
    std::string_view status;
    bool truncated{};
    std::span<const bootstrap_attempt_summary> attempts;
};

struct qualification_capture_evidence final {
    bool raw_usb_requested{};
    bool raw_usb_enabled{};
    bool has_touch_metadata{};
    bool button_labels_complete{};
    bool descriptor_fields_complete{};
    bool native_forward_complete{};
    bool native_forward_truncated{};
    bool callback_unlabeled_activity{};
    bool native_unlabeled_activity{};
    std::size_t callback_records{};
    std::uint64_t callback_drops{};
    std::uint64_t callback_incomplete_records{};
    std::uint64_t callback_identity_failures{};
    std::uint64_t callback_timestamp_order_violations{};
    std::size_t native_records{};
    std::uint64_t native_incomplete_records{};
    std::uint64_t native_identity_failures{};
    std::uint64_t native_timestamp_order_violations{};
    std::uint64_t identity_pairs{};
    std::uint64_t payload_mismatches{};
    std::uint64_t gesture_payload_matches{};
    std::uint64_t callback_raw_reads_available{};
    std::uint64_t callback_raw_reads_missing{};
    std::uint64_t native_raw_reads_available{};
    std::uint64_t native_raw_reads_missing{};
    std::uint64_t raw_identity_pairs{};
    std::uint64_t raw_payload_mismatches{};
};

// Writes a native_capture object through the opening records_detail array.
// The caller writes record JSON elements and closes the array and object.
void write_native_capture_prefix(std::ostream& output, const native_capture_summary& summary);

// Writes an unsigned integer as a quoted, lowercase hexadecimal JSON string.
void write_hex_json(std::ostream& output, std::uint64_t value);

void write_dualsense_usb_touch_json(std::ostream& output, const dualsense_usb_touch_record& touch);

[[nodiscard]] bool dualsense_usb_touch_payload_equal(const dualsense_usb_touch_record& left,
    const dualsense_usb_touch_record& right) noexcept;

[[nodiscard]] bool reading_payload_equal(const reading_sample& left,
    const reading_sample& right) noexcept;

[[nodiscard]] bool reading_fields_complete(const reading_sample& reading,
    std::uint32_t descriptor_button_count,
    std::uint32_t descriptor_surface_count) noexcept;

void write_bootstrap_measurement_json(std::ostream& output,
    const bootstrap_measurement_summary& summary);

[[nodiscard]] bool bootstrap_attempt_capacity_available(std::size_t attempt_count) noexcept;

[[nodiscard]] std::uint64_t unlabeled_button_mask(std::uint32_t device_button_count,
    std::span<const std::int32_t> labels) noexcept;

// Detects changes in button states whose labels are None, Unknown, or absent.
[[nodiscard]] bool saw_unlabeled_button_transition(std::uint32_t device_button_count,
    std::span<const std::int32_t> labels,
    std::span<const std::uint64_t> state_masks) noexcept;

[[nodiscard]] bool has_any_touch_contacts(std::span<const std::uint32_t> contact_counts) noexcept;

[[nodiscard]] std::string_view classify_capture_qualification(
    const qualification_capture_evidence& evidence) noexcept;

} // namespace inputnext::input_qualification
