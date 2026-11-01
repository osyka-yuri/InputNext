#include "effects/effects.hpp"
#include "effects/progress_evaluation.hpp"
#include "gameinput/output/connections/backend_selection.hpp"
#include "gameinput/output/connections/manager.hpp"
#include "output/authority_fixture.hpp"
#include "output/controlled_native_sink.hpp"
#include "output/execution_scope.hpp"

#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {

using namespace std::chrono_literals;
namespace effects = inputnext::effects;
namespace output = inputnext::output;
namespace test_output = inputnext::test::output;

template <typename Result>
[[nodiscard]] auto checked(Result result) {
    if (!result.has_value()) {
        throw std::runtime_error{"Progress correlation fixture operation failed"};
    }
    return std::move(*result);
}

[[nodiscard]] effects::detail::progress_marker marker_for(const output::public_context context,
    const output::mutation_receipt& receipt) {
    return {.present = true,
        .kind = effects::detail::marker_kind::discrete,
        .external_context = {.runtime = context.identity.target.runtime,
            .id = context.identity.id.value + 1000U},
        .native_context = context,
        .target = context.identity.target,
        .publication = {.value = receipt.logical_version + 10000U},
        .logical_version = receipt.logical_version,
        .discrete = receipt,
        .selected_channels = output::rumble_mask};
}

[[nodiscard]] effects::detail::progress_marker marker_for(const output::public_context context,
    const output::body_playback_receipt& receipt) {
    return {.present = true,
        .kind = effects::detail::marker_kind::body,
        .external_context = {.runtime = context.identity.target.runtime,
            .id = context.identity.id.value + 1000U},
        .native_context = context,
        .target = context.identity.target,
        .publication = {.value = receipt.body.logical_version + 10000U},
        .logical_version = receipt.body.logical_version,
        .body = receipt};
}

class correlation_fixture final {
public:
    correlation_fixture() : execution(native.owner) {
        std::unique_ptr<output::native_sink> sink =
            std::make_unique<test_output::controlled_native_sink>(control);
        backend = checked(execution.register_backend(native.connection.target,
            sink,
            output::supported_channel_mask));
    }

    [[nodiscard]] output::public_context open() const {
        return checked(native.owner.open(native.connection.target));
    }

    [[nodiscard]] effects::detail::progress_marker submit(const output::public_context context,
        const std::uint64_t sequence,
        const float low) const {
        const auto request = test_output::rumble_request({.channels = output::rumble_mask,
            .sequence = sequence,
            .ttl = 5s,
            .low = low,
            .high = 0.3F});
        return marker_for(context, checked(native.owner.submit(context, request)));
    }

    void publish() {
        const auto snapshot = checked(native.owner.read_snapshot(native.connection.target));
        if (!execution.engine().publish(backend, snapshot).has_value()) {
            throw std::runtime_error{"Progress correlation publication failed"};
        }
    }

    void drive(const output::native_status result) {
        control->set_next_result({.status = result});
        const auto lane = checked(execution.engine().lane_for(backend));
        if (!execution.engine().drive_one(lane)) {
            throw std::runtime_error{"Progress correlation native attempt did not run"};
        }
    }

    [[nodiscard]] inputnext::gameinput_detail::managed_backend_observation observe() {
        return {.kind = inputnext::gameinput_detail::output_backend_kind::dualsense_usb,
            .execution = checked(execution.engine().observe(backend))};
    }

    [[nodiscard]] effects::progress progress(const effects::detail::progress_marker& marker) {
        return checked(effects::detail::evaluate_progress_observation(marker, observe()));
    }

    test_output::authority_fixture native;
    test_output::execution_engine_scope execution;
    std::shared_ptr<test_output::controlled_native_state> control =
        std::make_shared<test_output::controlled_native_state>();
    output::backend_handle backend{};
};

TEST_CASE("effects.progress_waits_for_a_new_marker_before_attributing_an_old_native_failure",
    "[integration][effects][progress][correlation]") {
    const auto failure =
        GENERATE(output::native_status::failed, output::native_status::unsupported);
    correlation_fixture fixture;
    const auto context = fixture.open();
    const auto first = fixture.submit(context, 1U, 0.2F);
    fixture.publish();
    fixture.drive(failure);
    CHECK(fixture.progress(first).stage == effects::progress_stage::not_deliverable);

    const auto second = fixture.submit(context, 2U, 0.7F);
    {
        const auto native = fixture.observe();
        REQUIRE(native.execution.progress.delivery == output::delivery_state::blocked);
        REQUIRE(native.execution.observed_logical_version < second.logical_version);
        REQUIRE(!native.execution.projection_current);
    }
    const auto pending = fixture.progress(second);
    CHECK(pending.stage == effects::progress_stage::dispatch_pending);
    CHECK(pending.completion_status == effects::status::pending);
    CHECK(pending.requested == second.publication);
    CHECK(pending.current.value == 0U);
    CHECK(pending.attempt_serial == 0U);

    fixture.publish();
    fixture.drive(output::native_status::accepted);
    const auto completed = fixture.progress(second);
    CHECK(completed.stage == effects::progress_stage::current_completed);
    CHECK(completed.current == second.publication);
    CHECK(completed.completion_status == effects::status::ok);
}

TEST_CASE("effects.progress_reports_a_superseded_marker_before_another_publication_failure",
    "[integration][effects][progress][correlation]") {
    correlation_fixture fixture;
    const auto first = fixture.submit(fixture.open(), 1U, 0.2F);
    fixture.publish();
    fixture.drive(output::native_status::failed);
    const auto second = fixture.submit(fixture.open(), 1U, 0.7F);
    fixture.publish();
    fixture.drive(output::native_status::unsupported);

    const auto superseded = fixture.progress(first);
    CHECK(superseded.stage == effects::progress_stage::superseded);
    CHECK(superseded.completion_status == effects::status::stale_effect);
    CHECK(superseded.current.value == 0U);
    const auto failed = fixture.progress(second);
    CHECK(failed.stage == effects::progress_stage::not_deliverable);
    CHECK(failed.completion_status == effects::status::unsupported);
}

TEST_CASE("effects.progress_does_not_reuse_failure_before_a_changed_projection_is_published",
    "[integration][effects][progress][correlation]") {
    correlation_fixture fixture;
    const auto context = fixture.open();
    const auto marker = fixture.submit(context, 1U, 0.2F);
    fixture.publish();
    fixture.drive(output::native_status::failed);

    output::submit_request leds{};
    leds.selected_channels = output::leds_mask;
    leds.sequence = 2U;
    leds.ttl = 5s;
    leds.values.leds.rgb = {1U, 2U, 3U};
    REQUIRE(fixture.native.owner.submit(context, leds).has_value());
    {
        const auto native = fixture.observe();
        REQUIRE(native.execution.observed_logical_version >= marker.logical_version);
        REQUIRE(!native.execution.projection_current);
    }
    const auto pending = fixture.progress(marker);
    CHECK(pending.stage == effects::progress_stage::dispatch_pending);
    CHECK(pending.completion_status == effects::status::pending);

    fixture.publish();
    fixture.drive(output::native_status::accepted);
    CHECK(fixture.progress(marker).stage == effects::progress_stage::current_completed);
}

TEST_CASE("effects.progress_keeps_a_current_failure_after_an_equal_body_renewal_is_observed",
    "[integration][effects][progress][correlation][body]") {
    correlation_fixture fixture;
    const auto context = fixture.open();
    output::body_request request{.sequence = 1U,
        .ttl = 5s,
        .preference = output::body_route_preference::classic,
        .fallback = {.low_frequency = 0.2F, .high_frequency = 0.3F}};
    const auto first =
        checked(fixture.native.owner.submit_body(context, request, output::body_playback_update{}));
    fixture.publish();
    fixture.drive(output::native_status::failed);
    const auto old_publication = fixture.observe().execution.progress.observed_publication;

    request.lease = first.body.identity.lease;
    request.audio = first.body.identity.audio;
    request.sequence = 2U;
    const auto renewed =
        checked(fixture.native.owner.submit_body(context, request, output::body_playback_update{}));
    const auto marker = marker_for(context, renewed);
    CHECK(fixture.progress(marker).stage == effects::progress_stage::dispatch_pending);
    fixture.publish();
    const auto native = fixture.observe();
    CHECK(native.execution.progress.observed_publication == old_publication);
    CHECK(native.execution.projection_current);
    CHECK(fixture.control->call_count() == 1U);
    const auto failed = fixture.progress(marker);
    CHECK(failed.stage == effects::progress_stage::not_deliverable);
    CHECK(failed.completion_status == effects::status::io_failed);
}

} // namespace
