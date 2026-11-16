#include "dinput/force_feedback/contribution.hpp"

#include "dinput/native_types.hpp"
#include "output/composition/composition.hpp"
#include "output/composition/internal_producer.hpp"
#include "output/composition/window_acquisition.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_submission.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <memory>
#include <utility>

namespace inputnext::dinput::force_feedback {
namespace {

using clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

inline constexpr auto publication_horizon = 100ms;
inline constexpr auto renewal_margin = 25ms;

[[nodiscard]] HRESULT map_output_error(const output::error error) noexcept {
    switch (error) {
    case output::error::resources:
    case output::error::capacity_exceeded:
    case output::error::identity_exhausted:
    case output::error::sequence_rejected:
    case output::error::revision_exhausted:
    case output::error::logical_version_exhausted:
        return DIERR_OUTOFMEMORY;
    case output::error::not_connected:
    case output::error::stale_context:
    case output::error::revoked:
    case output::error::expired:
    case output::error::stale_lease:
    case output::error::stale_revision:
        return DIERR_INPUTLOST;
    case output::error::invalid_argument:
    case output::error::deadline_overflow:
    case output::error::busy:
    case output::error::stale_audio:
        return DIERR_GENERIC;
    }
    return DIERR_GENERIC;
}

[[nodiscard]] bool terminal_cleanup_error(const output::error error) noexcept {
    return error == output::error::stale_context;
}

template <typename Value, typename Operation>
[[nodiscard]] output::result<Value> guarded_output(Operation&& operation) noexcept {
    try {
        return std::forward<Operation>(operation)();
    } catch (...) {
        return std::unexpected{output::error::resources};
    }
}

[[nodiscard]] std::uint64_t next_sequence(std::uint64_t& sequence) noexcept {
    if (sequence == 0U) {
        return 0U;
    }
    const auto current = sequence;
    sequence = current == std::numeric_limits<std::uint64_t>::max() ? 0U : current + 1U;
    return current;
}

} // namespace

HRESULT contribution::open(output::detail::composition& composition,
    std::shared_ptr<const output::detail::window_acquisition> token,
    const output::backend_handle backend,
    bool& drive_requested) noexcept {
    if (occupied()) {
        return DIERR_OUTOFMEMORY;
    }
    if (token == nullptr || !token->live() || !token->target().valid()) {
        return DIERR_INPUTLOST;
    }

    auto producer = composition.make_window_rumble(token, backend);
    if (!producer.has_value()) {
        return map_output_error(producer.error());
    }
    auto context =
        guarded_output<output::internal_context>([&] { return producer->open(token->target()); });
    if (!context.has_value()) {
        return map_output_error(context.error());
    }

    close_request_id_ = context->identity.id.value;
    context_ = *context;
    producer_.emplace(std::move(*producer));
    token_ = std::move(token);
    phase_ = phase::open;
    if (!token_->live()) {
        const auto cleanup = close(drive_requested);
        return FAILED(cleanup) ? cleanup : DIERR_INPUTLOST;
    }
    return S_OK;
}

HRESULT contribution::submit(const output::rumble_value value,
    const clock::time_point now,
    bool& drive_requested,
    const clock::time_point boundary,
    const mutation_policy policy) noexcept {
    if (phase_ != phase::open || !context_.has_value() || token_ == nullptr || !token_->live()) {
        return DIERR_INPUTLOST;
    }

    const auto remaining =
        boundary == clock::time_point::max()
            ? publication_horizon
            : std::chrono::duration_cast<std::chrono::milliseconds>(boundary - now);
    const auto ttl = std::min(publication_horizon, remaining);
    if (ttl <= 0ms) {
        return clear(drive_requested, policy);
    }
    if (policy == mutation_policy::revoke_on_failure && publication_.has_value() &&
        publication_->value == value && publication_->expires_at - now > renewal_margin &&
        publication_->expires_at <= now + ttl) {
        return S_OK;
    }

    const auto sequence = next_sequence(sequence_);
    if (sequence == 0U) {
        if (policy == mutation_policy::preserve_on_failure) {
            return DIERR_OUTOFMEMORY;
        }
        const auto closed = close(drive_requested);
        return FAILED(closed) ? closed : DIERR_OUTOFMEMORY;
    }
    const auto window = output::make_finite_submission_window(now, ttl);
    if (!window.has_value()) {
        return map_output_error(window.error());
    }
    output::submit_request request{};
    request.selected_channels = output::rumble_mask;
    request.sequence = sequence;
    request.ttl = window->ttl();
    request.values.rumble = value;
    auto submitted = guarded_output<output::mutation_receipt>(
        [&] { return producer_->submit(*context_, request, *window); });
    if (!submitted.has_value()) {
        const auto failure = map_output_error(submitted.error());
        if (policy == mutation_policy::preserve_on_failure) {
            return failure;
        }
        const auto closed = close(drive_requested);
        return FAILED(closed) ? closed : failure;
    }

    revisions_ = submitted->resulting_revisions;
    publication_ = publication{.value = value, .expires_at = window->expires_at()};
    drive_requested = true;
    return S_OK;
}

HRESULT contribution::clear(bool& drive_requested, const mutation_policy policy) noexcept {
    if (phase_ == phase::vacant) {
        return S_OK;
    }
    if (phase_ == phase::closing) {
        if (policy == mutation_policy::preserve_on_failure) {
            return DIERR_INPUTLOST;
        }
        return close(drive_requested);
    }
    if (!publication_.has_value() && policy == mutation_policy::revoke_on_failure) {
        return S_OK;
    }

    const auto sequence = next_sequence(sequence_);
    if (sequence == 0U) {
        if (policy == mutation_policy::preserve_on_failure) {
            return DIERR_OUTOFMEMORY;
        }
        const auto closed = close(drive_requested);
        return FAILED(closed) ? closed : DIERR_OUTOFMEMORY;
    }
    output::clear_request request{};
    request.selected_channels = output::rumble_mask;
    request.sequence = sequence;
    request.expected_revisions = revisions_;
    auto cleared = guarded_output<output::mutation_receipt>(
        [&] { return producer_->clear(*context_, request); });
    if (cleared.has_value()) {
        revisions_ = cleared->resulting_revisions;
        publication_.reset();
        drive_requested = true;
        return S_OK;
    }
    if (policy == mutation_policy::preserve_on_failure) {
        return map_output_error(cleared.error());
    }

    // A failed revision clear cannot prove that the old rumble is gone. Revoke only
    // this exact private owner and retain its close receipt until acknowledged.
    const auto closed = close(drive_requested);
    return FAILED(closed) ? closed : S_OK;
}

HRESULT contribution::close(bool& drive_requested) noexcept {
    if (phase_ == phase::vacant) {
        return S_OK;
    }
    phase_ = phase::closing;
    return progress_close(drive_requested);
}

HRESULT contribution::progress_close(bool& drive_requested) noexcept {
    if (receipt_.has_value()) {
        auto acknowledged = guarded_output<void>([&] { return producer_->acknowledge(*receipt_); });
        if (acknowledged.has_value() || terminal_cleanup_error(acknowledged.error())) {
            reset();
            drive_requested = true;
            return S_OK;
        }
        return map_output_error(acknowledged.error());
    }

    auto closed = guarded_output<output::close_receipt>(
        [&] { return producer_->close(*context_, close_request_id_); });
    if (!closed.has_value()) {
        if (terminal_cleanup_error(closed.error())) {
            reset();
            drive_requested = true;
            return S_OK;
        }
        return map_output_error(closed.error());
    }

    receipt_ = *closed;
    context_.reset();
    publication_.reset();
    drive_requested = true;
    auto acknowledged = guarded_output<void>([&] { return producer_->acknowledge(*receipt_); });
    if (acknowledged.has_value() || terminal_cleanup_error(acknowledged.error())) {
        reset();
        return S_OK;
    }
    return map_output_error(acknowledged.error());
}

void contribution::retire(bool& drive_requested) noexcept {
    if (phase_ == phase::vacant) {
        return;
    }
    if (!producer_.has_value()) {
        std::terminate();
    }
    output::context_identity identity{};
    if (context_.has_value()) {
        identity = context_->identity;
    } else if (receipt_.has_value()) {
        identity = receipt_->identity;
    } else {
        std::terminate();
    }
    producer_->retire_window_context(identity);
    reset();
    drive_requested = true;
}

const std::shared_ptr<const output::detail::window_acquisition>&
contribution::token() const noexcept {
    return token_;
}

bool contribution::matches(
    const std::shared_ptr<const output::detail::window_acquisition>& token_value) const noexcept {
    return occupied() && token_.get() == token_value.get();
}

bool contribution::occupied() const noexcept {
    return phase_ != phase::vacant;
}

bool contribution::closing() const noexcept {
    return phase_ == phase::closing;
}

output::context_identity contribution::identity() const noexcept {
    if (context_.has_value()) {
        return context_->identity;
    }
    return receipt_.has_value() ? receipt_->identity : output::context_identity{};
}

void contribution::reset() noexcept {
    producer_.reset();
    context_.reset();
    receipt_.reset();
    publication_.reset();
    token_.reset();
    revisions_ = {};
    sequence_ = 1U;
    close_request_id_ = 0U;
    phase_ = phase::vacant;
}

} // namespace inputnext::dinput::force_feedback
