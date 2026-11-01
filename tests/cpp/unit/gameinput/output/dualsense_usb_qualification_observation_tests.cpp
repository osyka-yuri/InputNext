#include "gameinput_probe/output/dualsense_usb_candidate_selector.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/device_metadata.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace inputnext::gameinput_detail {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] connection_target make_target(const std::uint8_t suffix) noexcept {
    connection_target target{};
    target.runtime.bytes[0U] = std::byte{0x52};
    target.runtime.bytes[1U] = static_cast<std::byte>(suffix);
    target.device.value = 4U;
    target.connection.value = 5U;
    return target;
}

TEST_CASE("gameinput.probe.usb_qualification_refreshes_positive_and_negative_observations",
    "[gameinput][probe][qualification][unit]") {
    const auto target = make_target(1U);
    const auto query_started = maintenance_time{100s};
    dualsense_usb_qualification_observation observation;
    observation.select(target, 7U);

    CHECK(observation.due(query_started));
    observation.observe(std::expected<bool, metadata_error>{true},
        {.started = query_started, .completed = query_started + 1ms});
    CHECK(observation.eligible(query_started + 249ms));
    CHECK_FALSE(observation.due(query_started + 249ms));
    CHECK_FALSE(observation.eligible(query_started + 250ms));
    CHECK(observation.due(query_started + 250ms));

    const auto negative_started = query_started + 250ms;
    observation.observe(std::expected<bool, metadata_error>{false},
        {.started = negative_started, .completed = negative_started + 1ms});
    CHECK_FALSE(observation.eligible(negative_started + 2ms));
    CHECK_FALSE(observation.due(negative_started + 249ms));
    CHECK(observation.due(negative_started + 250ms));
}

TEST_CASE("gameinput.probe.usb_qualification_errors_clear_positive_observations",
    "[gameinput][probe][qualification][unit]") {
    const auto target = make_target(1U);
    const auto started = maintenance_time{200s};
    dualsense_usb_qualification_observation observation;
    observation.select(target, 3U);
    observation.observe(std::expected<bool, metadata_error>{true},
        {.started = started, .completed = started + 1ms});
    REQUIRE(observation.eligible(started + 2ms));

    const auto error_started = started + 64ms;
    observation.observe(std::unexpected{metadata_error::busy},
        {.started = error_started, .completed = error_started + 1ms});
    CHECK_FALSE(observation.eligible(error_started + 2ms));
    CHECK_FALSE(observation.due(error_started + 249ms));
    CHECK(observation.due(error_started + 250ms));
}

TEST_CASE("gameinput.probe.usb_qualification_rejects_late_results_and_resets_changed_keys",
    "[gameinput][probe][qualification][unit]") {
    const auto target = make_target(1U);
    const auto started = maintenance_time{300s};
    dualsense_usb_qualification_observation observation;
    observation.select(target, 11U);
    observation.observe(std::expected<bool, metadata_error>{true},
        {.started = started, .completed = started + 250ms});
    CHECK_FALSE(observation.eligible(started + 251ms));
    CHECK(observation.due(started + 251ms));

    observation.observe(std::expected<bool, metadata_error>{true},
        {.started = started + 1s, .completed = started + 1s + 1ms});
    REQUIRE(observation.eligible(started + 1s + 2ms));
    observation.select(target, 12U);
    CHECK_FALSE(observation.eligible(started + 1s + 2ms));
    CHECK(observation.due(started + 1s + 2ms));

    observation.observe(std::expected<bool, metadata_error>{true},
        {.started = started + 2s, .completed = started + 2s + 1ms});
    REQUIRE(observation.eligible(started + 2s + 2ms));
    observation.select(make_target(2U), 12U);
    CHECK_FALSE(observation.eligible(started + 2s + 2ms));
    CHECK(observation.due(started + 2s + 2ms));
}

} // namespace
} // namespace inputnext::gameinput_detail
