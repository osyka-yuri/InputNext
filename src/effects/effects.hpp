#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_submission.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>

namespace inputnext {
class device_registry;
class gameinput_runtime;

namespace core {
class service;
struct selector;
} // namespace core

namespace gameinput_detail {
class runtime_output_access;
class gameinput_output_connections;
} // namespace gameinput_detail

namespace audio_detail {
class audio_service;
}

namespace output {
class authority;
}
} // namespace inputnext

namespace inputnext::effects {

inline constexpr std::size_t context_capacity = 32U;

enum class status : std::uint8_t {
    ok,
    invalid_argument,
    unavailable,
    stale_target,
    unsupported_target,
    unsupported_channel,
    scheduler_unavailable,
    busy,
    stale_lease,
    capacity_exceeded,
    stale_context,
    stale_effect,
    stale_sequence,
    source_retired,
    unsupported,
    io_failed,
    revoked,
    pending,
    no_data,
    not_admitted,
};

template <typename Value>
using result = std::expected<Value, status>;

struct context_handle final {
    runtime_id runtime{};
    std::uint64_t id{};
    friend bool operator==(const context_handle&, const context_handle&) noexcept = default;
};

struct publication_revision final {
    std::uint64_t value{};
    friend constexpr bool operator==(publication_revision, publication_revision) noexcept = default;
};

enum class context_state : std::uint8_t { open, revoked };
enum class close_disposition : std::uint8_t { normal, connection_retired };
enum class progress_stage : std::uint8_t {
    dispatch_pending,
    current_completed,
    superseded,
    connection_retired,
    unknown,
    not_deliverable,
};
enum class completion_class : std::uint8_t {
    none,
    setter_returned_current,
    raw_output_accepted_current,
};

using feature_mask = std::uint32_t;

namespace feature_flags {
inline constexpr feature_mask none = 0U;
inline constexpr feature_mask left_trigger = 0x01U;
inline constexpr feature_mask right_trigger = 0x02U;
inline constexpr feature_mask low_rumble = 0x04U;
inline constexpr feature_mask high_rumble = 0x08U;
inline constexpr feature_mask lightbar = 0x10U;
inline constexpr feature_mask player_leds = 0x20U;
inline constexpr feature_mask microphone_led = 0x40U;
inline constexpr feature_mask body_lease = 0x80U;
inline constexpr feature_mask hd_ready = 0x100U;
} // namespace feature_flags

[[nodiscard]] constexpr bool has_feature(feature_mask flags, feature_mask required) noexcept {
    return (flags & required) == required;
}

enum class completion_capabilities : std::uint8_t {
    none = 0U,
    setter_returned_current = 0x01U,
    raw_output_accepted_current = 0x02U,
};

struct capabilities final {
    output::channel_mask supported_channels{};
    feature_mask features{feature_flags::none};
    completion_capabilities completion{completion_capabilities::none};
    std::uint64_t audio_instance_id{};
    std::uint64_t observation_revision{};
};

struct resolution final {
    connection_target target{};
    capabilities value{};
};

struct context_info final {
    context_handle context{};
    context_state state{context_state::open};
};

struct discrete_receipt final {
    context_handle context{};
    output::channel_mask accepted_mask{};
    std::uint64_t sequence{};
    publication_revision publication{};
    output::channel_revisions channel_revisions{};
};

struct body_request final {
    context_handle context{};
    std::uint64_t lease_id{};
    std::uint64_t audio_instance_id{};
    std::uint64_t sequence{};
    std::chrono::milliseconds ttl{};
    output::body_route_preference preference{output::body_route_preference::classic};
    output::rumble_value fallback{};
    output::body_pcm_command pcm_command{output::body_pcm_command::keep};
    std::optional<output::body_pcm_input> replacement = std::nullopt;
};

struct body_clear_request final {
    context_handle context{};
    std::uint64_t lease_id{};
    std::uint64_t audio_instance_id{};
    std::uint64_t sequence{};
};

struct body_receipt final {
    context_handle context{};
    connection_target target{};
    std::uint64_t lease_id{};
    std::uint64_t audio_instance_id{};
    std::uint64_t sequence{};
    publication_revision publication{};
    output::body_actuator_route route{output::body_actuator_route::classic};
    output::body_fallback_reason fallback{output::body_fallback_reason::none};
};

struct close_receipt final {
    close_disposition disposition{close_disposition::normal};
    context_handle context{};
    connection_target target{};
    std::uint64_t close_request_id{};
    publication_revision publication{};
    friend bool operator==(const close_receipt&, const close_receipt&) noexcept = default;
};

struct progress final {
    progress_stage stage{progress_stage::unknown};
    completion_class completion{completion_class::none};
    status completion_status{status::no_data};
    connection_target target{};
    publication_revision requested{};
    publication_revision current{};
    std::uint64_t attempt_serial{};
};

struct cleanup_report final {
    bool admission_closed{};
    std::size_t occupied_contexts{};
    std::size_t pending_native_cleanup{};
    std::optional<status> failure = std::nullopt;
};

class identity_source;

struct service_dependencies final {
    runtime_id identity{};
    device_registry& registry;
    output::authority& authority;
    core::service& core;
    gameinput_runtime& runtime;
    gameinput_detail::runtime_output_access& output_access;
    gameinput_detail::gameinput_output_connections& connections;
    audio_detail::audio_service& audio;
    identity_source& identities;
};

class service final {
public:
    explicit service(service_dependencies dependencies);
    ~service() noexcept;
    service(const service&) = delete;
    service& operator=(const service&) = delete;
    service(service&&) = delete;
    service& operator=(service&&) = delete;

    [[nodiscard]] result<resolution> resolve(const core::selector& selection);
    [[nodiscard]] result<context_handle> open(connection_target target);
    [[nodiscard]] result<context_info> inspect(context_handle context);
    [[nodiscard]] result<discrete_receipt> submit(context_handle context,
        const output::submit_request& request,
        const output::finite_submission_window& window);
    [[nodiscard]] status clear(context_handle context, const output::clear_request& request);
    [[nodiscard]] result<body_receipt> submit_body(const body_request& request,
        const output::finite_submission_window& window);
    [[nodiscard]] status clear_body(const body_clear_request& request);
    [[nodiscard]] result<close_receipt> close(context_handle context,
        std::uint64_t external_close_key);
    [[nodiscard]] status acknowledge(const close_receipt& receipt);
    [[nodiscard]] result<progress> query_progress(connection_target target,
        publication_revision publication);
    void close_admission() noexcept;
    [[nodiscard]] cleanup_report maintain() noexcept;
    [[nodiscard]] cleanup_report revoke_for_shutdown() noexcept;

private:
    struct implementation;
    std::unique_ptr<implementation> implementation_;
};

} // namespace inputnext::effects
