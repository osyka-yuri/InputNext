#pragma once

#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>
#include <inputnext/gameinput_runtime.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace inputnext::gameinput_detail {

class rumble_session;

struct dualsense_usb_candidate final {
    connection_target target{};
    std::uint64_t catalogue_generation{};
    metadata_device_model model{metadata_device_model::dualsense};
};

class dualsense_usb_qualification_observation final {
public:
    static constexpr auto refresh_interval = std::chrono::milliseconds{250};

    struct query_timing final {
        maintenance_time started;
        maintenance_time completed;
    };

    void select(connection_target target, std::uint64_t catalogue_generation) noexcept {
        if (target_ == target && catalogue_generation_ == catalogue_generation) {
            return;
        }
        target_ = target;
        catalogue_generation_ = catalogue_generation;
        eligible_.reset();
        next_attempt_ = maintenance_time::min();
    }

    [[nodiscard]] bool due(maintenance_time now) const noexcept { return now >= next_attempt_; }

    void observe(const metadata_result<bool>& result, const query_timing observation) noexcept {
        next_attempt_ = observation.started + refresh_interval;
        if (observation.completed >= next_attempt_) {
            eligible_.reset();
            next_attempt_ = observation.completed;
            return;
        }
        eligible_ = result.has_value() ? std::optional{*result} : std::nullopt;
    }

    [[nodiscard]] bool eligible(maintenance_time now) const noexcept {
        return eligible_ == true && now < next_attempt_;
    }

private:
    connection_target target_{};
    std::uint64_t catalogue_generation_{};
    std::optional<bool> eligible_;
    maintenance_time next_attempt_{maintenance_time::min()};
};

class dualsense_usb_candidate_selector final {
public:
    [[nodiscard]] std::optional<dualsense_usb_candidate> inspect(rumble_session& session,
        maintenance_time now);
    [[nodiscard]] bool is_eligible(rumble_session& session,
        connection_target target,
        std::uint64_t catalogue_generation,
        maintenance_time now);

private:
    struct qualification_entry final {
        connection_target target{};
        dualsense_usb_qualification_observation observation;
    };

    void retain_current(const connection_inventory& inventory);
    [[nodiscard]] qualification_entry& for_target(connection_target target,
        std::uint64_t catalogue_generation);
    [[nodiscard]] bool qualify_cached(rumble_session& session,
        connection_target target,
        std::uint64_t catalogue_generation,
        maintenance_time now);

    std::vector<qualification_entry> entries_;
};

} // namespace inputnext::gameinput_detail
