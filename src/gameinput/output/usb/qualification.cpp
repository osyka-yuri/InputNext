#include "qualification.hpp"

#include "gameinput/native/native_gameinput.hpp"
#include <inputnext/native_output.hpp>
#include <inputnext/output_execution.hpp>

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <new>
#include <utility>

namespace inputnext::gameinput_detail {
namespace {

class qualified_usb_sink final : public output::native_sink {
public:
    qualified_usb_sink(std::unique_ptr<output::native_sink> sink,
        std::shared_ptr<usb_output_qualification> qualification) noexcept
        : sink_(std::move(sink)),
          qualification_(std::move(qualification)) {}

    [[nodiscard]] output::native_result write(
        const output::native_command& command) noexcept override {
        if (!qualification_->admits(command.backend.target,
                usb_output_qualification::clock::now())) {
            return output::native_result{.status = output::native_status::retryable_failure};
        }
        return sink_->write(command);
    }

private:
    std::unique_ptr<output::native_sink> sink_;
    std::shared_ptr<usb_output_qualification> qualification_;
};

} // namespace

usb_output_qualification::usb_output_qualification(const connection_target target,
    const std::uint64_t catalogue_generation,
    const clock::time_point query_started) noexcept
    : target_(target),
      catalogue_generation_(catalogue_generation),
      expires_at_(ticks(query_started + validity)) {
}

bool usb_output_qualification::renew(const query_timing observation) noexcept {
    const auto new_expiry = ticks(observation.started + validity);
    if (observation.completed >= clock::time_point{clock::duration{new_expiry}}) {
        revoke();
        return false;
    }
    if (revoked_.load(std::memory_order_acquire)) {
        return false;
    }

    auto current_expiry = expires_at_.load(std::memory_order_acquire);
    for (;;) {
        if (observation.completed >= clock::time_point{clock::duration{current_expiry}}) {
            revoke();
            return false;
        }
        if (revoked_.load(std::memory_order_acquire)) {
            return false;
        }
        if (new_expiry <= current_expiry) {
            return true;
        }
        if (expires_at_.compare_exchange_weak(current_expiry,
                new_expiry,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return !revoked_.load(std::memory_order_acquire);
        }
    }
}

bool usb_output_qualification::admits(const connection_target target,
    const clock::time_point now) const noexcept {
    if (target != target_ || revoked_.load(std::memory_order_acquire)) {
        return false;
    }
    const auto expiry =
        clock::time_point{clock::duration{expires_at_.load(std::memory_order_acquire)}};
    if (now >= expiry) {
        // A query completed before expiry must not revive an already expired permit.
        revoke();
        return false;
    }
    return !revoked_.load(std::memory_order_acquire);
}

void usb_output_qualification::revoke() const noexcept {
    revoked_.store(true, std::memory_order_release);
}

native_result<std::unique_ptr<output::native_sink>> make_qualified_usb_sink(
    std::unique_ptr<output::native_sink> sink,
    std::shared_ptr<usb_output_qualification> qualification) noexcept {
    if (sink == nullptr || qualification == nullptr) {
        return std::unexpected{native_failure{.cause = native_failure_cause::native_failure}};
    }

    try {
        return std::unique_ptr<output::native_sink>{
            new qualified_usb_sink{std::move(sink), std::move(qualification)}};
    } catch (const std::bad_alloc&) {
        return std::unexpected{native_failure{.cause = native_failure_cause::resource_exhausted}};
    }
}

} // namespace inputnext::gameinput_detail
