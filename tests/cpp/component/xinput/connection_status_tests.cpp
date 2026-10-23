#include "support/xinput/test_ports.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/history.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_events.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <expected>

namespace {

using namespace inputnext;
using namespace inputnext::xinput_test_support;

class counting_event_source final : public xinput::event_source {
public:
    [[nodiscard]] history_result<history_reader_handle> open_history(
        [[maybe_unused]] std::uint32_t position,
        [[maybe_unused]] connection_target target,
        [[maybe_unused]] history_time now) noexcept override {
        ++calls_;
        return std::unexpected{history_failure{.code = history_error::provider_failure}};
    }

    [[nodiscard]] history_batch read_history([[maybe_unused]] history_reader_handle reader,
        [[maybe_unused]] history_time now) noexcept override {
        ++calls_;
        return history_batch{.failure = history_failure{.code = history_error::provider_failure}};
    }

    [[nodiscard]] history_result<void> close_history(
        [[maybe_unused]] history_reader_handle reader) noexcept override {
        ++calls_;
        return std::unexpected{history_failure{.code = history_error::provider_failure}};
    }

    [[nodiscard]] std::uint32_t calls() const noexcept { return calls_.load(); }

private:
    std::atomic<std::uint32_t> calls_;
};

} // namespace

TEST_CASE("xinput.connection_status_checks_only_index_assignment_and_live_binding",
    "[component][xinput][connection-status]") {
    registry_fixture devices;
    focus_fixture focus_state;
    metadata_source metadata;
    counting_event_source events;
    xinput::service service{focus_state.observer(), metadata, events, 1'000'000U};
    const auto connection = devices.add(gamepad_state{.buttons = 1U});
    const auto assigned =
        service.assign(connection.connection.target, connection.connection.reader);
    REQUIRE(assigned.has_value());

    CHECK(service.connection_status(4U) == ERROR_BAD_ARGUMENTS);
    CHECK(service.connection_status(assigned->user_index + 1U) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(service.connection_status(assigned->user_index) == ERROR_SUCCESS);

    XINPUT_STATE before{};
    REQUIRE(
        service.get_state(xinput::flavor::v1_4, assigned->user_index, &before) == ERROR_SUCCESS);
    CHECK(service.connection_status(assigned->user_index) == ERROR_SUCCESS);
    XINPUT_STATE after{};
    REQUIRE(service.get_state(xinput::flavor::v1_4, assigned->user_index, &after) == ERROR_SUCCESS);
    CHECK(after.dwPacketNumber == before.dwPacketNumber);
    CHECK(events.calls() == 0U);
    CHECK(metadata.calls() == 0U);

    REQUIRE(devices.registry().retire(connection.connection.target).has_value());
    CHECK(service.connection_status(assigned->user_index) == ERROR_DEVICE_NOT_CONNECTED);
    service.close();
    CHECK(service.connection_status(assigned->user_index) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(service.connection_status(4U) == ERROR_BAD_ARGUMENTS);
    CHECK(events.calls() == 0U);
    CHECK(metadata.calls() == 0U);
}
