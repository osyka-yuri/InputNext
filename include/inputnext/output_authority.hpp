#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_submission.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace inputnext::output {

struct authority_seed final {
    std::uint64_t next_context_id{1U};
    std::uint64_t logical_version{};
};

struct context_info final {
    context_identity identity{};
    std::uint64_t accepted_sequence{};
    channel_revisions revisions{};
    internal_priority priority{internal_priority::game_semantic};
    std::optional<focus::foreground_epoch> foreground;

    friend bool operator==(const context_info&, const context_info&) = default;
};

struct channel_provenance final {
    context_identity identity{};
    channel source_channel{};
    std::uint64_t revision{};
    std::uint64_t accepted_sequence{};
    std::uint64_t accepted_commit_order{};
    internal_priority priority{internal_priority::game_semantic};
    std::optional<focus::foreground_epoch> foreground = std::nullopt;

    friend bool operator==(const channel_provenance&, const channel_provenance&) = default;
};

template <typename Value>
struct channel_snapshot final {
    Value value;
    channel_provenance provenance;

    friend bool operator==(const channel_snapshot&, const channel_snapshot&) = default;
};

// An owned, coherent logical projection. OFF and zero-valued intents remain present typed
// winners; absent channels remain empty optionals.
struct logical_snapshot final {
    connection_target target{};
    authority_incarnation incarnation{};
    std::uint64_t logical_version{};
    std::chrono::steady_clock::time_point evaluated_at;
    // Minimum deadline among every surviving eligible active target contribution.
    std::optional<std::chrono::steady_clock::time_point> earliest_expiry;
    std::optional<channel_snapshot<trigger_value>> left_trigger;
    std::optional<channel_snapshot<trigger_value>> right_trigger;
    std::optional<channel_snapshot<rumble_value>> rumble;
    std::optional<channel_snapshot<led_value>> leds;
    std::optional<body_snapshot> body;

    [[nodiscard]] constexpr channel_mask present_channels() const noexcept {
        return (left_trigger.has_value() ? left_trigger_mask : 0U) |
               (right_trigger.has_value() ? right_trigger_mask : 0U) |
               ((rumble.has_value() || body.has_value()) ? rumble_mask : 0U) |
               (leds.has_value() ? leds_mask : 0U);
    }

    friend bool operator==(const logical_snapshot&, const logical_snapshot&) = default;
};

struct maintenance_report final {
    connection_target target{};
    authority_incarnation incarnation{};
    std::uint64_t logical_version{};
    std::size_t expired_channels{};
    std::size_t closed_contexts{};
    std::size_t reclaimed_contexts{};
    std::chrono::steady_clock::time_point evaluated_at;
};

namespace detail {
class internal_producer;
class xinput_rumble;
class xinput_rumble_transaction;
class authority_test_access;
class authority_execution_access;
class audio_route_control;
enum class incarnation_creation : std::uint8_t;
} // namespace detail

class authority final {
public:
    // Borrows the registry and focus observer through destruction. Keep both alive and quiesce
    // callers before teardown; each construction receives a fresh owner incarnation, while a
    // composition owns its authority for that composition's lifetime.
    authority(runtime_id runtime,
        device_registry& registry,
        focus::observer& focus_observer,
        authority_seed seed = {});
    ~authority() noexcept;

    authority(const authority&) = delete;
    authority& operator=(const authority&) = delete;
    authority(authority&&) = delete;
    authority& operator=(authority&&) = delete;

    [[nodiscard]] result<public_context> open(connection_target target);
    [[nodiscard]] result<mutation_receipt> submit(public_context context,
        const submit_request& request);
    [[nodiscard]] result<mutation_receipt> submit(public_context context,
        const submit_request& request,
        const finite_submission_window& window);
    [[nodiscard]] result<mutation_receipt> clear(public_context context,
        const clear_request& request);
    [[nodiscard]] result<body_receipt> submit_body(public_context context,
        const body_request& request);
    [[nodiscard]] result<body_playback_receipt> submit_body(public_context context,
        const body_request& request,
        const body_playback_update& playback);
    [[nodiscard]] result<body_playback_receipt> submit_body(public_context context,
        const body_request& request,
        const body_playback_update& playback,
        const finite_submission_window& window);
    [[nodiscard]] body_pcm_preparer pcm_preparer() const noexcept;
    // Moves completed and retired sample owners out of the authority gate. Destruction occurs
    // after the gate is released, so callers may use this as ordinary control progress.
    void collect_body_pcm() noexcept;
    [[nodiscard]] result<void> clear_body(public_context context,
        const body_clear_request& request);
    // Returns stored open-record state and performs local TTL maintenance. The opening foreground
    // epoch is historical; inspection does not requalify focus or connection liveness and does
    // not authorize execution or a later operation.
    [[nodiscard]] result<context_info> inspect(public_context context);
    [[nodiscard]] result<audio_route_observation> inspect_audio(audio_instance instance) const;
    [[nodiscard]] result<close_receipt> close(public_context context,
        std::uint64_t close_request_id);
    // Acknowledges an exact retained public receipt. Trusted internal receipts are acknowledged
    // only by their matching internal producer.
    [[nodiscard]] result<void> acknowledge(const close_receipt& receipt);
    [[nodiscard]] result<logical_snapshot> read_snapshot(connection_target target);
    [[nodiscard]] result<maintenance_report> maintain(connection_target target);

private:
    friend class detail::internal_producer;
    friend class detail::xinput_rumble;
    friend class detail::xinput_rumble_transaction;
    friend class detail::authority_test_access;
    friend class detail::authority_execution_access;
    friend class detail::audio_route_control;

    authority(runtime_id runtime,
        device_registry& registry,
        focus::observer& focus_observer,
        authority_seed seed,
        detail::incarnation_creation creation);

    struct implementation;
    std::unique_ptr<implementation> implementation_;

    [[nodiscard]] result<internal_context> open_internal(connection_target target,
        const internal_context_options& options);
    [[nodiscard]] result<mutation_receipt> submit_internal(internal_context context,
        const submit_request& request,
        const internal_context_options& options);
    [[nodiscard]] result<mutation_receipt> clear_internal(internal_context context,
        const clear_request& request,
        const internal_context_options& options);
    [[nodiscard]] result<close_receipt> close_internal(internal_context context,
        std::uint64_t close_request_id,
        const internal_context_options& options);
    [[nodiscard]] result<void> acknowledge_internal(const close_receipt& receipt,
        const internal_context_options& options);
    [[nodiscard]] result<body_playback_receipt> submit_body_impl(public_context context,
        const body_request& request,
        const body_playback_update& playback,
        const finite_submission_window* window);
};

} // namespace inputnext::output
