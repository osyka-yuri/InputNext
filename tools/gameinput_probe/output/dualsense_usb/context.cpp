#include "gameinput_probe/output/dualsense_usb/context.hpp"

#include "gameinput_probe/output/session.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <limits>
#include <utility>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] output::result<std::uint64_t> next_sequence(std::uint64_t& sequence) noexcept {
    if (sequence == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected{output::error::sequence_rejected};
    }
    return ++sequence;
}

} // namespace

output::result<void> dualsense_usb_context::open(output_session& session,
    const connection_target target) {
    if (is_open()) {
        return std::unexpected{output::error::invalid_argument};
    }
    auto producer = session.make_dualsense_diagnostic_producer();
    if (!producer.has_value()) {
        return std::unexpected{producer.error()};
    }
    auto context = producer->open(target);
    if (!context.has_value()) {
        return std::unexpected{context.error()};
    }
    producer_.emplace(std::move(*producer));
    context_ = *context;
    latest_receipt_.reset();
    sequence_ = 0U;
    return {};
}

bool dualsense_usb_context::is_open() const noexcept {
    return context_.has_value() && producer_.has_value();
}

const output::internal_context* dualsense_usb_context::context() const noexcept {
    return context_.has_value() ? &*context_ : nullptr;
}

const output::mutation_receipt* dualsense_usb_context::latest_receipt() const noexcept {
    return latest_receipt_.has_value() ? &*latest_receipt_ : nullptr;
}

output::result<output::mutation_receipt> dualsense_usb_context::submit(output_session& session,
    const output::channel_values values,
    const std::chrono::milliseconds ttl) {
    if (!context_.has_value() || !producer_.has_value()) {
        return std::unexpected{output::error::stale_context};
    }
    const auto sequence = next_sequence(sequence_);
    if (!sequence.has_value()) {
        return std::unexpected{sequence.error()};
    }
    auto receipt = producer_->submit(*context_,
        {.selected_channels = output::supported_channel_mask,
            .sequence = *sequence,
            .ttl = ttl,
            .values = values});
    if (receipt.has_value()) {
        latest_receipt_ = *receipt;
        session.request_drive();
    }
    return receipt;
}

output::result<output::mutation_receipt> dualsense_usb_context::clear(output_session& session) {
    if (!context_.has_value() || !producer_.has_value() || !latest_receipt_.has_value()) {
        return std::unexpected{output::error::stale_context};
    }
    const auto sequence = next_sequence(sequence_);
    if (!sequence.has_value()) {
        return std::unexpected{sequence.error()};
    }
    auto receipt = producer_->clear(*context_,
        {.selected_channels = output::supported_channel_mask,
            .sequence = *sequence,
            .expected_revisions = latest_receipt_->resulting_revisions});
    if (receipt.has_value()) {
        latest_receipt_ = *receipt;
        session.request_drive();
    }
    return receipt;
}

dualsense_usb_context_close_result dualsense_usb_context::close_and_acknowledge() {
    dualsense_usb_context_close_result result{};
    if (!context_.has_value() || !producer_.has_value()) {
        return result;
    }
    const auto receipt = producer_->close(*context_, context_->identity.id.value);
    if (receipt.has_value()) {
        result.close_submitted = true;
        result.acknowledged = producer_->acknowledge(*receipt).has_value();
    }
    context_.reset();
    latest_receipt_.reset();
    producer_.reset();
    sequence_ = 0U;
    return result;
}

} // namespace inputnext::gameinput_detail
