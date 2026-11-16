#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>

#include <atomic>

namespace inputnext::output::detail {

class window_acquisition final {
public:
    window_acquisition(connection_target target, focus::window_epoch epoch) noexcept
        : target_(target),
          epoch_(epoch) {}

    ~window_acquisition() = default;

    window_acquisition(const window_acquisition&) = delete;
    window_acquisition& operator=(const window_acquisition&) = delete;
    window_acquisition(window_acquisition&&) = delete;
    window_acquisition& operator=(window_acquisition&&) = delete;

    [[nodiscard]] connection_target target() const noexcept { return target_; }

    [[nodiscard]] const focus::window_epoch& epoch() const noexcept { return epoch_; }

    [[nodiscard]] bool live() const noexcept { return live_.load(std::memory_order_acquire); }

    void revoke() const noexcept { live_.store(false, std::memory_order_release); }

private:
    const connection_target target_;
    const focus::window_epoch epoch_;
    mutable std::atomic<bool> live_{true};
};

} // namespace inputnext::output::detail
