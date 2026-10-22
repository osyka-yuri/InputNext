#pragma once

#include <inputnext/output.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <optional>

#include <inputnext/device_identity.hpp>

namespace inputnext::output {

struct execution_id final {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0U; }

    friend constexpr bool operator==(execution_id, execution_id) noexcept = default;
};

struct backend_id final {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0U; }

    friend constexpr bool operator==(backend_id, backend_id) noexcept = default;
};

struct publication_id final {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0U; }

    friend constexpr bool operator==(publication_id, publication_id) noexcept = default;
};

struct attempt_id final {
    std::uint64_t value{};

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0U; }

    friend constexpr bool operator==(attempt_id, attempt_id) noexcept = default;
};

struct execution_owner final {
    authority_incarnation authority{};
    execution_id id{};

    [[nodiscard]] constexpr bool valid() const noexcept { return authority.valid() && id.valid(); }

    friend constexpr bool operator==(const execution_owner&,
        const execution_owner&) noexcept = default;
};

struct backend_incarnation final {
    execution_owner owner{};
    backend_id id{};

    [[nodiscard]] constexpr bool valid() const noexcept { return owner.valid() && id.valid(); }

    friend constexpr bool operator==(const backend_incarnation&,
        const backend_incarnation&) noexcept = default;
};

struct backend_handle final {
    connection_target target{};
    backend_incarnation incarnation{};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return target.valid() && incarnation.valid();
    }

    friend constexpr bool operator==(const backend_handle&,
        const backend_handle&) noexcept = default;
};

enum class execution_error : std::uint8_t {
    invalid_argument,
    not_connected,
    resources,
    capacity_exceeded,
    identity_exhausted,
    already_attached,
    not_running,
    stale_backend,
    backend_conflict,
    busy,
    pending,
};

template <typename Value>
using execution_result = std::expected<Value, execution_error>;

enum class native_status : std::uint8_t {
    accepted,
    retryable_failure,
    unsupported,
    failed,
    disconnected,
};

struct native_result final {
    native_status status{native_status::failed};
    std::int64_t native_code{};

    friend constexpr bool operator==(const native_result&, const native_result&) noexcept = default;
};

struct completed_attempt final {
    attempt_id id{};
    publication_id publication{};
    native_result result{};

    friend constexpr bool operator==(const completed_attempt&,
        const completed_attempt&) noexcept = default;
};

enum class retirement_reason : std::uint8_t {
    requested,
    replaced,
    disconnected,
    connection_retired,
    shutdown,
    identity_exhausted,
};

struct retirement_receipt final {
    backend_handle backend{};
    retirement_reason reason{retirement_reason::requested};

    friend constexpr bool operator==(const retirement_receipt&,
        const retirement_receipt&) noexcept = default;
};

enum class backend_phase : std::uint8_t {
    current,
    retiring,
    cleaning,
    drained,
};

enum class delivery_state : std::uint8_t {
    pending,
    in_flight,
    setter_accepted,
    retry_wait,
    blocked,
    identity_exhausted,
};

struct backend_progress final {
    backend_handle backend{};
    backend_phase phase{backend_phase::current};
    channel_mask represented_channels{};
    channel_mask unsupported_channels{};
    publication_id observed_publication{};
    delivery_state delivery{delivery_state::pending};
    // Qualified retryable outcomes for this publication, retained as history after retirement.
    std::uint32_t consecutive_retryable_failures{};
    // Nominal delay for the pending automatic retry, not time remaining.
    std::chrono::milliseconds scheduled_retry_delay{};
    // Identifies the newest admitted attempt, which may still be in flight.
    std::optional<attempt_id> last_attempt;
    // Carries its own identity because it may describe an older attempt than last_attempt.
    std::optional<completed_attempt> last_completed_attempt;
    bool write_in_flight{};
    bool retry_requested{};
};

} // namespace inputnext::output
