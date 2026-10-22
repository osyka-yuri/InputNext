#include "support/xinput/service_test_access.hpp"

#include "focus/observer_test_access.hpp"
#include "output/composition/composition.hpp"
#include "output/composition/internal_producer.hpp"
#include "output/xinput/rumble.hpp"
#include "support/focus/controlled_source.hpp"
#include "support/focus/manual_clock.hpp"
#include "support/xinput/test_ports.hpp"
#include <inputnext/focus.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using namespace inputnext;
using namespace inputnext::xinput_test_support;
using output_error = inputnext::output::error;

struct configured_service final {
    registry_fixture devices;
    focus_fixture focus_state;
    metadata_source metadata;
    test_connection connection{devices.add()};
    output::detail::composition composition{connection.connection.target.runtime,
        devices.registry(),
        focus_state.observer()};
    xinput::service service{focus_state.observer(),
        metadata,
        take_bridge(composition),
        xinput::presentation_seed{}};

    [[nodiscard]] static std::unique_ptr<output::detail::xinput_rumble> take_bridge(
        output::detail::composition& root) {
        auto created = root.make_xinput_rumble();
        if (!created.has_value()) {
            throw std::runtime_error{"could not create XInput output bridge"};
        }
        return std::move(created.value());
    }

    [[nodiscard]] xinput::assignment assign() {
        const auto assigned =
            service.assign(connection.connection.target, connection.connection.reader);
        if (!assigned.has_value()) {
            throw std::runtime_error{"could not assign output test connection"};
        }
        return assigned.value();
    }

    [[nodiscard]] output::logical_snapshot snapshot() {
        const auto current =
            composition.public_authority().read_snapshot(connection.connection.target);
        if (!current.has_value()) {
            throw std::runtime_error{"could not read output test snapshot"};
        }
        return current.value();
    }
};

[[nodiscard]] DWORD
set_state(xinput::service& service, DWORD position, const XINPUT_VIBRATION& vibration) {
    return service.set_state(xinput::flavor::v1_4, position, &vibration);
}

TEST_CASE("xinput.output_normalizes_both_motors_and_keeps_zero_present",
    "[component][xinput][output]") {
    configured_service fixture{};
    const auto occupant = fixture.assign();
    CHECK(set_state(fixture.service,
              occupant.user_index,
              XINPUT_VIBRATION{.wLeftMotorSpeed = 65535U, .wRightMotorSpeed = 32768U}) ==
          ERROR_SUCCESS);

    const auto submitted = fixture.snapshot();
    REQUIRE(submitted.rumble.has_value());
    const auto submitted_rumble =
        submitted.rumble.value_or(output::channel_snapshot<output::rumble_value>{});
    CHECK(submitted_rumble.value.low_frequency == 1.0F);
    CHECK(submitted_rumble.value.high_frequency == static_cast<float>(32768U) / 65535.0F);
    CHECK(submitted_rumble.provenance.identity.kind == output::owner_class::trusted_internal);

    CHECK(set_state(fixture.service,
              occupant.user_index,
              XINPUT_VIBRATION{.wLeftMotorSpeed = 0U, .wRightMotorSpeed = 0U}) == ERROR_SUCCESS);
    const auto stopped = fixture.snapshot();
    REQUIRE(stopped.rumble.has_value());
    const auto stopped_rumble =
        stopped.rumble.value_or(output::channel_snapshot<output::rumble_value>{});
    CHECK(stopped_rumble.value == output::rumble_value{});
    const auto status = fixture.service.get_rumble_output_status(occupant.user_index);
    REQUIRE(status.has_value());
    const auto output_status = status.value_or(xinput::rumble_output_status{});
    CHECK(output_status.configured);
    CHECK(output_status.desired_state_cached);
    CHECK((!output_status.restore_pending));
    CHECK(output_status.request_revision == 2U);
    CHECK(output_status.logical_version == stopped.logical_version);
}

TEST_CASE("xinput.output_disabled_cache_keeps_only_latest_and_restores_zero",
    "[component][xinput][output]") {
    configured_service fixture{};
    const auto occupant = fixture.assign();
    CHECK(set_state(fixture.service,
              occupant.user_index,
              XINPUT_VIBRATION{.wLeftMotorSpeed = 9000U, .wRightMotorSpeed = 19000U}) ==
          ERROR_SUCCESS);

    fixture.service.enable(FALSE);
    CHECK((!fixture.snapshot().rumble.has_value()));
    CHECK(set_state(fixture.service,
              occupant.user_index,
              XINPUT_VIBRATION{.wLeftMotorSpeed = 30000U, .wRightMotorSpeed = 1000U}) ==
          ERROR_SUCCESS);
    CHECK(set_state(fixture.service,
              occupant.user_index,
              XINPUT_VIBRATION{.wLeftMotorSpeed = 0U, .wRightMotorSpeed = 0U}) == ERROR_SUCCESS);
    const auto cached = fixture.service.get_rumble_output_status(occupant.user_index);
    REQUIRE(cached.has_value());
    const auto cached_status = cached.value_or(xinput::rumble_output_status{});
    CHECK(cached_status.desired_state_cached);
    CHECK(cached_status.request_revision == 3U);
    CHECK((!fixture.snapshot().rumble.has_value()));

    fixture.service.enable(TRUE);
    const auto restored = fixture.snapshot();
    REQUIRE(restored.rumble.has_value());
    const auto restored_rumble =
        restored.rumble.value_or(output::channel_snapshot<output::rumble_value>{});
    CHECK(restored_rumble.value == output::rumble_value{});
    const auto status = fixture.service.get_rumble_output_status(occupant.user_index);
    REQUIRE(status.has_value());
    const auto output_status = status.value_or(xinput::rumble_output_status{});
    CHECK((!output_status.restore_pending));
    CHECK(output_status.last_restore_error == ERROR_SUCCESS);
}

TEST_CASE("xinput.output_background_and_stale_capture_do_not_grant_new_rights",
    "[component][xinput][output][concurrency]") {
    configured_service fixture{};
    const auto occupant = fixture.assign();
    const XINPUT_VIBRATION vibration{.wLeftMotorSpeed = 1U, .wRightMotorSpeed = 2U};

    xinput::detail::service_test_access::prepared_rumble prepared{};
    REQUIRE(xinput::detail::service_test_access::prepare_rumble(fixture.service,
                occupant.user_index,
                &vibration,
                prepared) == ERROR_SUCCESS);
    fixture.service.enable(FALSE);
    CHECK(xinput::detail::service_test_access::commit_rumble(fixture.service,
              occupant.user_index,
              prepared) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK((!fixture.service.get_rumble_output_status(occupant.user_index)
            .value_or(xinput::rumble_output_status{})
            .desired_state_cached));

    fixture.service.enable(TRUE);
    REQUIRE(xinput::detail::service_test_access::prepare_rumble(fixture.service,
                occupant.user_index,
                &vibration,
                prepared) == ERROR_SUCCESS);
    fixture.focus_state.set(focus::evidence::background);
    CHECK(xinput::detail::service_test_access::commit_rumble(fixture.service,
              occupant.user_index,
              prepared) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK((!fixture.snapshot().rumble.has_value()));

    fixture.focus_state.set(focus::evidence::foreground);
    REQUIRE(xinput::detail::service_test_access::prepare_rumble(fixture.service,
                occupant.user_index,
                &vibration,
                prepared) == ERROR_SUCCESS);
    REQUIRE(fixture.service.unassign(occupant).has_value());
    const auto replacement = fixture.assign();
    CHECK(replacement.target == occupant.target);
    CHECK(replacement.id != occupant.id);
    CHECK(xinput::detail::service_test_access::commit_rumble(fixture.service,
              replacement.user_index,
              prepared) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK((!fixture.snapshot().rumble.has_value()));

    fixture.focus_state.set(focus::evidence::background);
    CHECK(set_state(fixture.service,
              occupant.user_index,
              XINPUT_VIBRATION{.wLeftMotorSpeed = 4U, .wRightMotorSpeed = 5U}) == ERROR_SUCCESS);
    const auto background = fixture.service.get_rumble_output_status(occupant.user_index);
    REQUIRE(background.has_value());
    const auto background_status = background.value_or(xinput::rumble_output_status{});
    CHECK((!background_status.desired_state_cached));
}

TEST_CASE("xinput.output_enable_identity_exhaustion_still_revokes_and_cleans_up",
    "[component][xinput][output]") {
    configured_service fixture{};
    const auto occupant = fixture.assign();
    CHECK(set_state(fixture.service,
              occupant.user_index,
              XINPUT_VIBRATION{.wLeftMotorSpeed = 20000U, .wRightMotorSpeed = 30000U}) ==
          ERROR_SUCCESS);
    xinput::detail::service_test_access::seed_event_enable_generation(fixture.service,
        std::numeric_limits<std::uint64_t>::max());

    fixture.service.enable(FALSE);
    CHECK((!fixture.snapshot().rumble.has_value()));
    const auto status = fixture.service.get_rumble_output_status(occupant.user_index);
    REQUIRE(status.has_value());
    const auto output_status = status.value_or(xinput::rumble_output_status{});
    CHECK((!output_status.desired_state_cached));
    CHECK(xinput::detail::service_test_access::event_enable_generation(fixture.service) ==
          std::numeric_limits<std::uint64_t>::max());
    CHECK(set_state(fixture.service,
              occupant.user_index,
              XINPUT_VIBRATION{.wLeftMotorSpeed = 1U, .wRightMotorSpeed = 1U}) ==
          ERROR_DEVICE_NOT_CONNECTED);
}

TEST_CASE("xinput.output_captured_foreground_epoch_expires_before_commit",
    "[component][xinput][output][concurrency]") {
    registry_fixture devices;
    const auto connection = devices.add();
    test::focus::manual_clock clock;
    const focus::process_instance process{.pid = 456U, .creation_time_100ns = 998877U};
    const std::vector<focus::reading> readings{
        {.process = process, .state = focus::evidence::foreground}};
    auto controlled = std::make_unique<test::focus::controlled_source>(process, readings, 2U);
    auto* const source = controlled.get();
    const auto focus_owner =
        focus::detail::observer_test_access::make_with_clock(make_runtime_id(0x62U),
            std::move(controlled),
            &test::focus::manual_clock::read,
            &clock);

    struct release_guard final {
        explicit release_guard(test::focus::controlled_source& source_value) noexcept
            : source(source_value) {}

        release_guard(const release_guard&) = delete;
        release_guard& operator=(const release_guard&) = delete;
        release_guard(release_guard&&) = delete;
        release_guard& operator=(release_guard&&) = delete;

        ~release_guard() { source.release(); }

        test::focus::controlled_source& source;
    };

    const release_guard release_source{*source};

    REQUIRE(source->wait_until_blocked());

    metadata_source metadata;
    output::detail::composition composition{connection.connection.target.runtime,
        devices.registry(),
        *focus_owner};
    auto bridge = composition.make_xinput_rumble();
    REQUIRE(bridge.has_value());
    xinput::service service{*focus_owner,
        metadata,
        std::move(bridge.value()),
        xinput::presentation_seed{}};
    const auto occupant =
        service.assign(connection.connection.target, connection.connection.reader);
    REQUIRE(occupant.has_value());

    const XINPUT_VIBRATION vibration{.wLeftMotorSpeed = 100U, .wRightMotorSpeed = 200U};
    xinput::detail::service_test_access::prepared_rumble prepared{};
    REQUIRE(xinput::detail::service_test_access::prepare_rumble(service,
                occupant.value().user_index,
                &vibration,
                prepared) == ERROR_SUCCESS);
    clock.advance(std::chrono::milliseconds{250});
    CHECK(xinput::detail::service_test_access::commit_rumble(service,
              occupant.value().user_index,
              prepared) == ERROR_DEVICE_NOT_CONNECTED);
    const auto status = service.get_rumble_output_status(occupant.value().user_index);
    REQUIRE(status.has_value());
    const auto output_status = status.value_or(xinput::rumble_output_status{});
    CHECK((!output_status.desired_state_cached));
}

TEST_CASE("xinput.output_same_target_reassignment_does_not_inherit_owner_or_cache",
    "[component][xinput][output]") {
    configured_service fixture{};
    const auto first = fixture.assign();
    fixture.service.enable(FALSE);
    CHECK(set_state(fixture.service,
              first.user_index,
              XINPUT_VIBRATION{.wLeftMotorSpeed = 1234U, .wRightMotorSpeed = 5678U}) ==
          ERROR_SUCCESS);
    REQUIRE(fixture.service.unassign(first).has_value());
    const auto second = fixture.assign();
    CHECK(second.user_index == first.user_index);
    CHECK(second.id != first.id);
    const auto status = fixture.service.get_rumble_output_status(second.user_index);
    REQUIRE(status.has_value());
    const auto output_status = status.value_or(xinput::rumble_output_status{});
    CHECK((!output_status.desired_state_cached));
    CHECK((!fixture.snapshot().rumble.has_value()));
}

TEST_CASE("xinput.output_cleanup_preserves_independent_public_owner",
    "[component][xinput][output]") {
    configured_service fixture{};
    const auto occupant = fixture.assign();
    auto& authority = fixture.composition.public_authority();
    const auto public_context = authority.open(fixture.connection.connection.target);
    REQUIRE(public_context.has_value());
    output::submit_request public_request{};
    public_request.selected_channels = output::rumble_mask;
    public_request.sequence = 1U;
    public_request.ttl = std::chrono::seconds{60};
    public_request.values.rumble = {.low_frequency = 0.2F, .high_frequency = 0.4F};
    REQUIRE(authority.submit(public_context.value(), public_request).has_value());

    CHECK(set_state(fixture.service,
              occupant.user_index,
              XINPUT_VIBRATION{.wLeftMotorSpeed = 50000U, .wRightMotorSpeed = 60000U}) ==
          ERROR_SUCCESS);
    REQUIRE(fixture.service.unassign(occupant).has_value());
    const auto remaining = fixture.snapshot();
    REQUIRE(remaining.rumble.has_value());
    const auto remaining_rumble =
        remaining.rumble.value_or(output::channel_snapshot<output::rumble_value>{});
    CHECK(remaining_rumble.value == public_request.values.rumble);
    CHECK(remaining_rumble.provenance.identity.kind == output::owner_class::public_game);
}

TEST_CASE("xinput.output_restore_failure_is_bounded_and_maintain_retries_latest",
    "[component][xinput][output]") {
    configured_service fixture{};
    const auto occupant = fixture.assign();
    fixture.service.enable(FALSE);
    CHECK(
        set_state(fixture.service,
            occupant.user_index,
            XINPUT_VIBRATION{.wLeftMotorSpeed = 0U, .wRightMotorSpeed = 42000U}) == ERROR_SUCCESS);

    std::vector<output::detail::internal_producer> fillers;
    std::vector<output::internal_context> contexts;
    fillers.reserve(32U);
    contexts.reserve(32U);
    for (std::uint64_t index = 1U; index <= 32U; ++index) {
        const output::internal_context_options options{.owner = output::internal_owner_key{index},
            .priority = output::internal_priority::generic_profile,
            .ttl = output::ttl_permission::finite_or_indefinite,
            .foreground = output::foreground_policy::background};
        auto created = fixture.composition.make_producer(options);
        REQUIRE(created.has_value());
        fillers.push_back(std::move(created.value()));
        auto context = fillers.back().open(fixture.connection.connection.target);
        REQUIRE(context.has_value());
        contexts.push_back(context.value());
    }

    fixture.service.enable(TRUE);
    auto status = fixture.service.get_rumble_output_status(occupant.user_index);
    REQUIRE(status.has_value());
    auto output_status = status.value_or(xinput::rumble_output_status{});
    CHECK(output_status.desired_state_cached);
    CHECK(output_status.restore_pending);
    CHECK(output_status.last_restore_error == ERROR_NOT_ENOUGH_MEMORY);
    CHECK((!fixture.snapshot().rumble.has_value()));

    const auto receipt =
        fillers.front().close(contexts.front(), contexts.front().identity.id.value);
    REQUIRE(receipt.has_value());
    REQUIRE(fillers.front().acknowledge(receipt.value()).has_value());
    fixture.service.maintain_output();

    status = fixture.service.get_rumble_output_status(occupant.user_index);
    REQUIRE(status.has_value());
    output_status = status.value_or(xinput::rumble_output_status{});
    CHECK(output_status.desired_state_cached);
    CHECK((!output_status.restore_pending));
    CHECK(output_status.last_restore_error == ERROR_SUCCESS);
    const auto restored = fixture.snapshot();
    REQUIRE(restored.rumble.has_value());
    const auto restored_rumble =
        restored.rumble.value_or(output::channel_snapshot<output::rumble_value>{});
    CHECK(restored_rumble.value.high_frequency == static_cast<float>(42000U) / 65535.0F);
}

TEST_CASE("xinput.output_poweroff_reports_only_the_frozen_supported_contract",
    "[component][xinput][output]") {
    registry_fixture devices;
    focus_fixture focus_state;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto connection = devices.add();
    const auto assigned =
        service.assign(connection.connection.target, connection.connection.reader);
    REQUIRE(assigned.has_value());
    CHECK(service.power_off_controller(xinput::flavor::v1_4, 4U) == ERROR_BAD_ARGUMENTS);
    CHECK(service.power_off_controller(xinput::flavor::v1_4, assigned.value().user_index) ==
          ERROR_NOT_SUPPORTED);
    CHECK(service.power_off_controller(xinput::flavor::v1_4, assigned.value().user_index + 1U) ==
          ERROR_DEVICE_NOT_CONNECTED);
    REQUIRE(devices.registry().retire(connection.connection.target).has_value());
    CHECK(service.power_off_controller(xinput::flavor::v1_4, assigned.value().user_index) ==
          ERROR_DEVICE_NOT_CONNECTED);
}

TEST_CASE("xinput.output_unconfigured_constructor_and_argument_results_stay_explicit",
    "[component][xinput][output]") {
    registry_fixture devices;
    focus_fixture focus_state;
    metadata_source metadata;
    xinput::service service{focus_state.observer(), metadata};
    const auto connection = devices.add();
    const auto assigned =
        service.assign(connection.connection.target, connection.connection.reader);
    REQUIRE(assigned.has_value());
    const XINPUT_VIBRATION vibration{.wLeftMotorSpeed = 1U, .wRightMotorSpeed = 1U};
    CHECK(service.set_state(xinput::flavor::v1_4, 4U, &vibration) == ERROR_BAD_ARGUMENTS);
    CHECK(service.set_state(xinput::flavor::v1_4, assigned.value().user_index, nullptr) ==
          ERROR_BAD_ARGUMENTS);
    CHECK(service.set_state(xinput::flavor::v1_4, assigned.value().user_index, &vibration) ==
          ERROR_NOT_SUPPORTED);
    const auto status = service.get_rumble_output_status(assigned.value().user_index);
    REQUIRE(status.has_value());
    const auto output_status = status.value_or(xinput::rumble_output_status{});
    CHECK((!output_status.configured));
    CHECK((!output_status.desired_state_cached));
    CHECK((!output_status.restore_pending));
}

TEST_CASE("xinput.output_unconfigured_set_state_requires_configuration_in_every_focus_state",
    "[component][xinput][output]") {
    SECTION("foreground and disabled service") {
        registry_fixture devices;
        focus_fixture focus_state;
        metadata_source metadata;
        xinput::service service{focus_state.observer(), metadata};
        const auto connection = devices.add();
        const auto assigned =
            service.assign(connection.connection.target, connection.connection.reader);
        REQUIRE(assigned.has_value());
        const XINPUT_VIBRATION vibration{.wLeftMotorSpeed = 111U, .wRightMotorSpeed = 222U};

        CHECK(set_state(service, assigned->user_index, vibration) == ERROR_NOT_SUPPORTED);
        service.enable(FALSE);
        CHECK(set_state(service, assigned->user_index, vibration) == ERROR_NOT_SUPPORTED);
        CHECK(set_state(service, 4U, vibration) == ERROR_BAD_ARGUMENTS);
        CHECK(set_state(service, assigned->user_index, XINPUT_VIBRATION{}) == ERROR_NOT_SUPPORTED);
    }

    SECTION("background") {
        registry_fixture devices;
        focus_fixture focus_state;
        metadata_source metadata;
        xinput::service service{focus_state.observer(), metadata};
        const auto connection = devices.add();
        const auto assigned =
            service.assign(connection.connection.target, connection.connection.reader);
        REQUIRE(assigned.has_value());
        focus_state.set(focus::evidence::background);
        const XINPUT_VIBRATION vibration{.wLeftMotorSpeed = 333U, .wRightMotorSpeed = 444U};

        CHECK(set_state(service, assigned->user_index, vibration) == ERROR_NOT_SUPPORTED);
        CHECK(set_state(service, 4U, vibration) == ERROR_BAD_ARGUMENTS);
        CHECK(service.set_state(xinput::flavor::v1_4, assigned->user_index, nullptr) ==
              ERROR_BAD_ARGUMENTS);
        xinput::detail::service_test_access::prepared_rumble prepared{};
        prepared.known_background = true;
        CHECK(xinput::detail::service_test_access::commit_rumble(service,
                  assigned->user_index,
                  prepared) == ERROR_NOT_SUPPORTED);
    }

    SECTION("unavailable focus") {
        registry_fixture devices;
        focus_fixture focus_state;
        metadata_source metadata;
        xinput::service service{focus_state.observer(), metadata};
        const auto connection = devices.add();
        const auto assigned =
            service.assign(connection.connection.target, connection.connection.reader);
        REQUIRE(assigned.has_value());
        focus_state.observer().stop();
        const XINPUT_VIBRATION vibration{.wLeftMotorSpeed = 555U, .wRightMotorSpeed = 666U};

        CHECK(set_state(service, assigned->user_index, vibration) == ERROR_NOT_SUPPORTED);
        CHECK(set_state(service, 4U, vibration) == ERROR_BAD_ARGUMENTS);
        CHECK(service.set_state(xinput::flavor::v1_4, assigned->user_index, nullptr) ==
              ERROR_BAD_ARGUMENTS);
    }

    SECTION("disconnected binding takes precedence over focus and configuration") {
        registry_fixture devices;
        focus_fixture focus_state;
        metadata_source metadata;
        xinput::service service{focus_state.observer(), metadata};
        const auto connection = devices.add();
        const auto assigned =
            service.assign(connection.connection.target, connection.connection.reader);
        REQUIRE(assigned.has_value());
        focus_state.set(focus::evidence::background);
        REQUIRE(devices.registry().retire(connection.connection.target).has_value());
        const XINPUT_VIBRATION vibration{.wLeftMotorSpeed = 777U, .wRightMotorSpeed = 888U};

        CHECK(set_state(service, assigned->user_index, vibration) == ERROR_DEVICE_NOT_CONNECTED);
    }
}

TEST_CASE("xinput.output_configured_background_does_not_cache_a_request",
    "[component][xinput][output]") {
    configured_service fixture{};
    const auto occupant = fixture.assign();
    fixture.focus_state.set(focus::evidence::background);
    const XINPUT_VIBRATION vibration{.wLeftMotorSpeed = 9876U, .wRightMotorSpeed = 5432U};

    CHECK(set_state(fixture.service, occupant.user_index, vibration) == ERROR_SUCCESS);
    const auto status = fixture.service.get_rumble_output_status(occupant.user_index);
    REQUIRE(status.has_value());
    const auto output_status = status.value_or(xinput::rumble_output_status{});
    CHECK((!output_status.desired_state_cached));
    CHECK((!output_status.restore_pending));
    CHECK(output_status.request_revision == 0U);
    CHECK((!fixture.snapshot().rumble.has_value()));
}

TEST_CASE("xinput.output_bridge_requires_the_same_presentation_observer",
    "[component][xinput][output]") {
    registry_fixture devices;
    focus_fixture authority_focus;
    focus_fixture unrelated_focus;
    metadata_source metadata;
    const auto connection = devices.add();
    output::detail::composition composition{connection.connection.target.runtime,
        devices.registry(),
        authority_focus.observer()};
    auto bridge = composition.make_xinput_rumble();
    REQUIRE(bridge.has_value());
    CHECK_THROWS_AS((xinput::service{unrelated_focus.observer(),
                        metadata,
                        std::move(bridge.value()),
                        xinput::presentation_seed{}}),
        std::invalid_argument);
}

TEST_CASE("xinput.output_request_revision_exhaustion_preserves_the_previous_intent",
    "[component][xinput][output]") {
    configured_service fixture{};
    const auto occupant = fixture.assign();
    CHECK(set_state(fixture.service,
              occupant.user_index,
              XINPUT_VIBRATION{.wLeftMotorSpeed = 12000U, .wRightMotorSpeed = 43000U}) ==
          ERROR_SUCCESS);
    const auto before = fixture.snapshot();
    REQUIRE(before.rumble.has_value());

    xinput::detail::service_test_access::seed_rumble_request_revision(fixture.service,
        occupant,
        std::numeric_limits<std::uint64_t>::max());
    CHECK(set_state(fixture.service,
              occupant.user_index,
              XINPUT_VIBRATION{.wLeftMotorSpeed = 40000U, .wRightMotorSpeed = 5000U}) ==
          ERROR_GEN_FAILURE);
    const auto after = fixture.snapshot();
    REQUIRE(after.rumble.has_value());
    const auto after_rumble =
        after.rumble.value_or(output::channel_snapshot<output::rumble_value>{});
    const auto before_rumble =
        before.rumble.value_or(output::channel_snapshot<output::rumble_value>{});
    CHECK(after_rumble.value == before_rumble.value);
    const auto status = fixture.service.get_rumble_output_status(occupant.user_index);
    REQUIRE(status.has_value());
    const auto output_status = status.value_or(xinput::rumble_output_status{});
    CHECK(output_status.request_revision == std::numeric_limits<std::uint64_t>::max());
}

} // namespace
