#include <inputnext/input_binding.hpp>

#include "connection_record.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/error.hpp>
#include <inputnext/input_sample.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <mutex>
#include <utility>

namespace inputnext {
namespace {

[[nodiscard]] result<void> validate_capture_admission(const detail::connection_record& record,
    connection_target target) noexcept {
    if (record.phase.load(std::memory_order_seq_cst) != detail::connection_phase::live ||
        record.target != target) {
        return std::unexpected{error::not_connected};
    }
    if (record.pin_count.load(std::memory_order_seq_cst) ==
        std::numeric_limits<std::size_t>::max()) {
        return std::unexpected{error::capacity_exceeded};
    }
    return {};
}

} // namespace

read_pin::read_pin(detail::connection_record* record, connection_target target) noexcept
    : record_(record),
      target_(target) {
}

read_pin::~read_pin() {
    release();
}

read_pin::read_pin(read_pin&& other) noexcept
    : record_(std::exchange(other.record_, nullptr)),
      target_(std::exchange(other.target_, connection_target{})) {
}

read_pin& read_pin::operator=(read_pin&& other) noexcept {
    if (this != &other) {
        release();
        record_ = std::exchange(other.record_, nullptr);
        target_ = std::exchange(other.target_, connection_target{});
    }
    return *this;
}

result<connection_snapshot> read_pin::read() const {
    if (record_ == nullptr || !target_.valid()) {
        return std::unexpected{error::not_connected};
    }

    // The pin keeps latest engaged; latest_value::read synchronizes with publication and close.
    if (!record_->latest.has_value()) {
        return std::unexpected{error::not_connected};
    }
    const auto sample = record_->latest->read();
    if (!sample.has_value()) {
        return std::unexpected{sample.error()};
    }
    return connection_snapshot{.target = target_, .sample = sample.value()};
}

connection_target read_pin::target() const noexcept {
    return target_;
}

void read_pin::release() noexcept {
    auto* const record = std::exchange(record_, nullptr);
    target_ = connection_target{};
    if (record == nullptr) {
        return;
    }

    if (record->release_pin()) {
        record->notify_pin_release();
    }
}

read_binding::read_binding(detail::connection_record* record, connection_target target) noexcept
    : record_(record),
      target_(target) {
}

result<read_pin> read_binding::capture() const {
    if (record_ == nullptr || !target_.valid()) {
        return std::unexpected{error::invalid_argument};
    }

    std::scoped_lock lock{record_->gate};
    const auto admission = validate_capture_admission(*record_, target_);
    if (!admission.has_value()) {
        return std::unexpected{admission.error()};
    }

    record_->pin_count.fetch_add(1U, std::memory_order_seq_cst);
    return read_pin{record_, target_};
}

result<void> read_binding::validate_capture() const {
    if (record_ == nullptr || !target_.valid()) {
        return std::unexpected{error::invalid_argument};
    }

    std::scoped_lock lock{record_->gate};
    return validate_capture_admission(*record_, target_);
}

result<connection_snapshot> read_binding::read() const {
    auto pin = capture();
    if (!pin.has_value()) {
        return std::unexpected{pin.error()};
    }
    return pin->read();
}

producer_binding::producer_binding(detail::connection_record* record,
    connection_target target) noexcept
    : record_(record),
      target_(target) {
}

result<std::uint64_t> producer_binding::publish(const input_update& update) const {
    if (record_ == nullptr || !target_.valid()) {
        return std::unexpected{error::invalid_argument};
    }

    std::scoped_lock lock{record_->gate};
    if (record_->phase.load(std::memory_order_seq_cst) != detail::connection_phase::live ||
        record_->target != target_ || !record_->latest.has_value()) {
        return std::unexpected{error::not_connected};
    }
    return record_->latest->publish(update);
}

} // namespace inputnext
