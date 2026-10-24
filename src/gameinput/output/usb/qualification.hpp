#pragma once

#include "gameinput/native/native_gameinput.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/native_output.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>

namespace inputnext::gameinput_detail {

class usb_output_qualification final {
public:
    using clock = std::chrono::steady_clock;

    static constexpr auto refresh_interval = std::chrono::milliseconds{64};
    static constexpr auto validity = std::chrono::milliseconds{256};

    struct query_timing final {
        clock::time_point started;
        clock::time_point completed;
    };

    usb_output_qualification(connection_target target,
        std::uint64_t catalogue_generation,
        clock::time_point query_started) noexcept;
    ~usb_output_qualification() = default;

    usb_output_qualification(const usb_output_qualification&) = delete;
    usb_output_qualification& operator=(const usb_output_qualification&) = delete;
    usb_output_qualification(usb_output_qualification&&) = delete;
    usb_output_qualification& operator=(usb_output_qualification&&) = delete;

    [[nodiscard]] connection_target target() const noexcept { return target_; }

    [[nodiscard]] std::uint64_t catalogue_generation() const noexcept {
        return catalogue_generation_;
    }

    [[nodiscard]] bool renew(query_timing observation) noexcept;
    [[nodiscard]] bool admits(connection_target target, clock::time_point now) const noexcept;
    void revoke() const noexcept;

private:
    using tick = clock::duration::rep;

    [[nodiscard]] static constexpr tick ticks(clock::time_point value) noexcept {
        return value.time_since_epoch().count();
    }

    const connection_target target_;
    const std::uint64_t catalogue_generation_;
    std::atomic<tick> expires_at_;
    mutable std::atomic<bool> revoked_{false};
};

[[nodiscard]] native_result<std::unique_ptr<output::native_sink>> make_qualified_usb_sink(
    std::unique_ptr<output::native_sink> sink,
    std::shared_ptr<usb_output_qualification> qualification) noexcept;

} // namespace inputnext::gameinput_detail
